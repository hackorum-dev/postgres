# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Verify that a standby does not remove tuple versions visible to backends that
# import snapshots during conflict resolution.  During recovery, the standby
# determines which VXIDs conflict with recovery and waits for these VXIDs to
# resolve.  If it checks only once for conflicting VXIDs, it may miss a
# conflicting VXID created later by a backend importing a snapshot -- an
# invisible conflict leading to incorrect results for the importing backend.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf(
	'postgresql.conf', qq[
autovacuum = off
]);
$node_primary->start;

my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);

# max_standby_streaming_delay = -1 makes replay wait rather than cancel.  The
# conflict reports are not diagnostic: the test reads them to tell which
# backends each scan found.
$node_standby->append_conf(
	'postgresql.conf', qq[
hot_standby_feedback = off
max_standby_streaming_delay = -1
log_recovery_conflict_waits = on
deadlock_timeout = 10ms
]);
$node_standby->start;

$node_primary->safe_psql(
	'postgres', qq[
CREATE TABLE t (id int);
INSERT INTO t SELECT generate_series(1, 100);
]);
$node_primary->wait_for_replay_catchup($node_standby);

is($node_standby->safe_psql('postgres', 'SELECT count(*) FROM t'),
	100, 'standby sees 100 rows before conflict');

# S1: Register an xmin and export its snapshot.
my $s1 = $node_standby->background_psql('postgres', on_error_stop => 0);
$s1->query_safe(qq[BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ;]);
my $s1_count = $s1->query_safe(qq[SELECT count(*) FROM t;]);
like($s1_count, qr/^100$/m, 'S1 sees 100 rows, snapshot registered');
my $snapid = $s1->query_safe(qq[SELECT pg_export_snapshot();]);
$snapid =~ s/^\s+|\s+$//g;
note("exported snapshot id: $snapid");

# S1's backend_xmin as seen on the standby.
my $s1_pid = $s1->query_safe(qq[SELECT pg_backend_pid();]);
$s1_pid =~ s/^\s+|\s+$//g;
my $s1_xmin = $node_standby->safe_psql('postgres',
	qq[SELECT backend_xmin FROM pg_stat_activity WHERE pid = $s1_pid;]);
note("S1 pid=$s1_pid backend_xmin=$s1_xmin");

# Primary: delete and vacuum so the prune record conflicts with S1.
my $log_offset = -s $node_standby->logfile;
my $lsn_before_vac =
  $node_primary->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
$node_primary->safe_psql(
	'postgres', qq[
DELETE FROM t;
VACUUM t;
]);
my $lsn_after_vac =
  $node_primary->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
note("primary WAL: before vacuum=$lsn_before_vac after=$lsn_after_vac");

# The first scan must find exactly S1.
$log_offset = $node_standby->wait_for_log(qr/Conflicting process: $s1_pid\./,
	$log_offset);

my $replay_lsn =
  $node_standby->safe_psql('postgres', 'SELECT pg_last_wal_replay_lsn()');
note("standby replay stalled at $replay_lsn (primary at $lsn_after_vac)");

# S2: a new backend imports S1's snapshot
my $s2 = $node_standby->background_psql('postgres', on_error_stop => 0);
$s2->query_safe(qq[BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ;]);
$s2->query_safe(qq[SET TRANSACTION SNAPSHOT '$snapid';]);

my $s2_pid = $s2->query_safe(qq[SELECT pg_backend_pid();]);
$s2_pid =~ s/^\s+|\s+$//g;
my $s2_xmin = $node_standby->safe_psql('postgres',
	qq[SELECT backend_xmin FROM pg_stat_activity WHERE pid = $s2_pid;]);
note("S2 pid=$s2_pid backend_xmin=$s2_xmin (S1 xmin was $s1_xmin)");
is($s2_xmin, $s1_xmin, "S2 adopted S1's xmin");

# S1 exits, draining the set the first scan found.
$s1->query_safe(qq[COMMIT;]);
$s1->quit;

# Wait until the standby commits to one behavior or the other: it either
# rescans and reports S2, or it resumes replay past the prune record.  Waiting
# only for the rescan would turn the wrong-results case into a timeout instead
# of a failed assertion.
my $s2_conflict = qr/Conflicting process: $s2_pid\./;
foreach (1 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
{
	last if $node_standby->log_contains($s2_conflict, $log_offset);
	my $advanced = $node_standby->safe_psql('postgres',
		qq[SELECT pg_last_wal_replay_lsn() >= '$lsn_after_vac'::pg_lsn]);
	last if $advanced eq 't';
	usleep(100_000);
}

ok($node_standby->log_contains($s2_conflict, $log_offset),
	'second scan found S2');

# Replay must not have advanced past the prune record.
my $replay_after_s1 =
  $node_standby->safe_psql('postgres', 'SELECT pg_last_wal_replay_lsn()');
note("replay after S1 exit: $replay_after_s1 (primary at $lsn_after_vac)");
my $stalled = $node_standby->safe_psql('postgres',
	qq[SELECT pg_last_wal_replay_lsn() < '$lsn_after_vac'::pg_lsn]);
is($stalled, 't', 'replay stalled while S2 holds the imported snapshot');

my $s2_result = $s2->query_safe(qq[SELECT count(*) FROM t;]);
$s2_result =~ s/^\s+|\s+$//g;

my $s2_alive = $s2->query_safe(qq[SELECT 'alive';]);
like($s2_alive, qr/alive/m, 'S2 not cancelled');

is($s2_result, '100', 'S2 sees all rows of its imported snapshot');

$s2->query_safe(qq[COMMIT;]);
$s2->quit;

$node_primary->wait_for_replay_catchup($node_standby);

is($node_standby->safe_psql('postgres', 'SELECT count(*) FROM t'),
	0, 'standby reflects the pruned table after catchup');

$node_standby->stop;
$node_primary->stop;

done_testing();
