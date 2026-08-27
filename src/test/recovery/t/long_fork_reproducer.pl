# Reproduce the Long Fork anomaly between a primary and standby.
# This is a standalone reproducer, not an entry in the recovery test schedule.
#
# Scenario:
#   * t0: x=0, y=0 on primary, standby1, and standby2.
#   * Pause replay on standby1.
#   * tx1 (synchronous_commit=remote_apply): UPDATE x=1; COMMIT. Waits on pause.
#   * Standby2 receives tx1's WAL and applies it.  We poll until standby2 has
#     reached the primary's current flush LSN, then pause standby2.  Standby2
#     now holds at (x=1, y=0).
#   * tx2 (synchronous_commit=off): UPDATE y=1; COMMIT.  Returns immediately.
#     tx2's WAL ships to both standbys but neither applies it (both paused).
#   * Read primary: a fresh snapshot sees tx1 as running and tx2 as committed,
#     returning (x=0, y=1).
#   * Read standby2: paused at the post-tx1 LSN, returning (x=1, y=0).
#
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;
$primary->backup('bk');

my $standby1 = PostgreSQL::Test::Cluster->new('standby1');
$standby1->init_from_backup($primary, 'bk', has_streaming => 1);
$standby1->start;

my $standby2 = PostgreSQL::Test::Cluster->new('standby2');
$standby2->init_from_backup($primary, 'bk', has_streaming => 1);
$standby2->start;

$primary->safe_psql('postgres',
	"ALTER SYSTEM SET synchronous_standby_names = 'standby1';");
$primary->reload;

$primary->poll_query_until('postgres', q[
    SELECT sync_state = 'sync'
    FROM pg_stat_replication
    WHERE application_name = 'standby1'
]) or die "standby1 never reached sync_state='sync'";

$primary->poll_query_until('postgres', q[
    SELECT count(*) = 1
    FROM pg_stat_replication
    WHERE application_name = 'standby2'
]) or die "standby2 never connected";

$primary->safe_psql('postgres', q[
    CREATE TABLE t (k text PRIMARY KEY, v int NOT NULL);
    INSERT INTO t VALUES ('x', 0), ('y', 0);
]);
$primary->wait_for_catchup($standby1);
$primary->wait_for_catchup($standby2);

$standby1->safe_psql('postgres', 'SELECT pg_wal_replay_pause();');

my $tx1 = $primary->background_psql('postgres');
$tx1->query_safe(q[
    SET application_name = 'long_fork_t1';
    SET synchronous_commit = 'remote_apply';
]);
$tx1->query_until(qr/committing/, qq[
BEGIN;
UPDATE t SET v = 1 WHERE k = 'x';
\\echo committing
COMMIT;
]);

$primary->poll_query_until('postgres', q[
    SELECT count(*) = 1
    FROM pg_stat_activity
    WHERE application_name = 'long_fork_t1'
      AND wait_event_type = 'IPC'
      AND wait_event = 'SyncRep'
]) or die "tx1 never reached SyncRep wait";

my $lsn = $primary->safe_psql('postgres',
	"SELECT pg_current_wal_flush_lsn();");
chomp $lsn;

$standby2->poll_query_until('postgres',
	"SELECT pg_last_wal_replay_lsn() >= '$lsn'::pg_lsn")
  or die "standby2 never applied tx1";

$standby2->safe_psql('postgres', 'SELECT pg_wal_replay_pause();');

$primary->safe_psql('postgres', q[
    SET synchronous_commit = off;
    BEGIN;
    UPDATE t SET v = 1 WHERE k = 'y';
    COMMIT;
]);

my $m = $primary->safe_psql('postgres',
	"SELECT string_agg(v::text, ',' ORDER BY k) FROM t;");
my $s = $standby2->safe_psql('postgres',
	"SELECT string_agg(v::text, ',' ORDER BY k) FROM t;");

is($m, '0,1', 'primary sees T2 but not T1');
is($s, '1,0', 'standby sees T1 but not T2');

$standby1->safe_psql('postgres', 'SELECT pg_wal_replay_resume();');
$standby2->safe_psql('postgres', 'SELECT pg_wal_replay_resume();');
$tx1->quit;
$primary->wait_for_catchup($standby1);
$primary->wait_for_catchup($standby2);

done_testing();
