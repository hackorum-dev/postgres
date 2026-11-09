
# Copyright (c) 2026, PostgreSQL Global Development Group

# Slot synchronization must keep a synced slot temporary until the standby
# can build a consistent snapshot from its restart_lsn.  Since commit
# 788ec96d591, update_and_persist_local_synced_slot() decides from
# slotsync_skip_reason, which a no-op sync cycle resets to SS_SKIP_NONE,
# so a slot that failed the consistency check in one cycle is persisted in
# the next one.  Once the primary slot advances, the next sync of that
# now-persistent slot hits Assert(persistency == RS_TEMPORARY) at
# slotsync.c:364.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 'logical');
$primary->append_conf('postgresql.conf', "autovacuum = off");
$primary->start;
$primary->safe_psql(
	'postgres', q{
	CREATE TABLE t (a int);
	SELECT pg_create_physical_replication_slot('phys');
	SELECT pg_create_logical_replication_slot('fslot', 'test_decoding',
											  false, false, true);
});

$primary->backup('bkp');
my $connstr = $primary->connstr;
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->append_conf(
	'postgresql.conf', qq{
hot_standby_feedback = on
primary_slot_name = 'phys'
primary_conninfo = '$connstr dbname=postgres'
});
$standby->start;
$primary->wait_for_replay_catchup($standby);

# Pause replay so that the standby never sees the xid assigned below;
# the synced slot's catalog_xmin then equals the primary slot's.
$standby->safe_psql('postgres', 'SELECT pg_wal_replay_pause()');

# An open transaction with an xid, and a running_xacts record listing it.
my $xact = $primary->background_psql('postgres');
my $xid = $xact->query_safe('BEGIN; SELECT pg_current_xact_id()');

# Each statement in its own transaction; the second get_changes moves the
# slot's restart_lsn to the running_xacts record above.
$primary->safe_psql('postgres', 'SELECT pg_log_standby_snapshot()');
$primary->safe_psql('postgres', 'INSERT INTO t VALUES (1)');
$primary->safe_psql('postgres',
	"SELECT pg_logical_slot_get_changes('fslot', NULL, NULL)") for 1 .. 2;

my ($catalog_xmin, $confirmed_flush) = split /\|/,
  $primary->safe_psql('postgres',
	"SELECT catalog_xmin, confirmed_flush_lsn FROM pg_replication_slots WHERE slot_name = 'fslot'"
  );
is($catalog_xmin, $xid, 'primary slot restart point lists the open xid');

$standby->poll_query_until('postgres',
	"SELECT pg_last_wal_receive_lsn() >= '$confirmed_flush'::pg_lsn");

# Synchronize.  The call blocks in SnapBuildWaitSnapshot() until the open
# transaction ends, then fails to reach a consistent point before the
# confirmed_flush_lsn and retries 200ms later.
my $log_offset = -s $standby->logfile;
my $sync = $standby->background_psql('postgres', on_error_stop => 0);
$sync->query_until(
	qr/start/, q{
	\echo start
	SELECT pg_sync_replication_slots();
});
$standby->safe_psql('postgres', 'SELECT pg_wal_replay_resume()');
$standby->wait_for_log(qr/Waiting for transactions/, $log_offset);
$xact->quit;

$standby->wait_for_log(qr/could not synchronize replication slot "fslot"/,
	$log_offset);
$standby->wait_for_log(
	qr/newly created replication slot "fslot" is sync-ready now/,
	$log_offset);
$sync->quit;

is( $standby->safe_psql(
		'postgres',
		"SELECT temporary FROM pg_replication_slots WHERE slot_name = 'fslot'"
	),
	't',
	'synced slot without a consistent snapshot stays temporary');

done_testing();
