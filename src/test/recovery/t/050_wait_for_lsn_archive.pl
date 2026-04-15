# Checks that WAIT FOR LSN works correctly during archive recovery,
# where no WAL receiver process exists.  Without the fix in
# GetCurrentLSNForWaitType(), standby_write and standby_flush modes
# would always time out because GetWalRcvWriteRecPtr()/
# GetWalRcvFlushRecPtr() return InvalidXLogRecPtr (0/0) when there
# is no WAL receiver.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Path qw(make_path);

# Initialize primary node with archiving enabled
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(has_archiving => 1, allows_streaming => 1);
$node_primary->start;

# Create test table and insert initial data
$node_primary->safe_psql('postgres',
	"CREATE TABLE wait_test AS SELECT generate_series(1,10) AS a");

# Take a backup for the archive standby
my $backup_name = 'archive_backup';
$node_primary->backup($backup_name);

# Create an archive-only standby (no streaming replication).
# We explicitly disable primary_conninfo so that no WAL receiver
# is started; WAL is obtained solely via restore_command.
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_restoring => 1);
$node_standby->start;

# Confirm that the standby is in recovery and has no WAL receiver
my $in_recovery =
  $node_standby->safe_psql('postgres', "SELECT pg_is_in_recovery()");
is($in_recovery, 't', "archive standby is in recovery");

my $walrcv_count = $node_standby->safe_psql('postgres',
	"SELECT count(*) FROM pg_stat_wal_receiver");
is($walrcv_count, '0', "archive standby has no WAL receiver");

# Insert data on primary and switch WAL to force archiving
$node_primary->safe_psql('postgres',
	"INSERT INTO wait_test VALUES (generate_series(11, 20))");
my $lsn1 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");
$node_primary->safe_psql('postgres', "SELECT pg_switch_wal()");

# Wait for archive standby to replay up to the target LSN
$node_standby->poll_query_until('postgres',
	"SELECT pg_last_wal_replay_lsn() >= '${lsn1}'::pg_lsn");

# 1. Test standby_replay mode - should succeed (this always worked)
my $output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn1}' WITH (MODE 'standby_replay', TIMEOUT '5s', NO_THROW);
]);
is($output, 'success',
	"archive standby: standby_replay returns success for replayed LSN");

# 2. Test standby_flush mode - this was broken before the fix
$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn1}' WITH (MODE 'standby_flush', TIMEOUT '5s', NO_THROW);
]);
is($output, 'success',
	"archive standby: standby_flush returns success for replayed LSN");

# 3. Test standby_write mode - this was broken before the fix
$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn1}' WITH (MODE 'standby_write', TIMEOUT '5s', NO_THROW);
]);
is($output, 'success',
	"archive standby: standby_write returns success for replayed LSN");

# 4. Verify data visibility after WAIT FOR on the archive standby
$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn1}' WITH (MODE 'standby_flush', TIMEOUT '5s');
	SELECT count(*) FROM wait_test;
]);
is((split("\n", $output))[-1],
	'20', "archive standby: data is visible after WAIT FOR standby_flush");

# 5. Test that an unreachable LSN still times out correctly
my $unreachable_lsn =
  $node_primary->safe_psql('postgres',
	"SELECT pg_current_wal_insert_lsn() + 10000000000");

$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${unreachable_lsn}' WITH (MODE 'standby_flush', TIMEOUT '100ms', NO_THROW);
]);
is($output, 'timeout',
	"archive standby: standby_flush correctly times out for unreachable LSN");

$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${unreachable_lsn}' WITH (MODE 'standby_write', TIMEOUT '100ms', NO_THROW);
]);
is($output, 'timeout',
	"archive standby: standby_write correctly times out for unreachable LSN");

$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${unreachable_lsn}' WITH (MODE 'standby_replay', TIMEOUT '100ms', NO_THROW);
]);
is($output, 'timeout',
	"archive standby: standby_replay correctly times out for unreachable LSN");

# 6. Test with a second batch of WAL - verify the fix works for
# incrementally archived and restored WAL, not just initial recovery.
$node_primary->safe_psql('postgres',
	"INSERT INTO wait_test VALUES (generate_series(21, 30))");
my $lsn2 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");
$node_primary->safe_psql('postgres', "SELECT pg_switch_wal()");

# Wait for archive standby to catch up
$node_standby->poll_query_until('postgres',
	"SELECT pg_last_wal_replay_lsn() >= '${lsn2}'::pg_lsn");

$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn2}' WITH (MODE 'standby_flush', TIMEOUT '5s', NO_THROW);
]);
is($output, 'success',
	"archive standby: standby_flush works for incrementally archived WAL");

$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn2}' WITH (MODE 'standby_write', TIMEOUT '5s', NO_THROW);
]);
is($output, 'success',
	"archive standby: standby_write works for incrementally archived WAL");

$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn2}' WITH (MODE 'standby_replay', TIMEOUT '5s');
	SELECT count(*) FROM wait_test;
]);
is((split("\n", $output))[-1],
	'30',
	"archive standby: all 30 rows visible after incremental archive recovery"
);

# 7. Test that primary_flush correctly errors on the archive standby
my $stderr;
$node_standby->psql(
	'postgres',
	"WAIT FOR LSN '${lsn2}' WITH (MODE 'primary_flush');",
	stderr => \$stderr);
ok($stderr =~ /recovery is in progress/,
	"archive standby: primary_flush correctly errors on standby");

# 8. Verify no WAL receiver appeared during the tests
$walrcv_count = $node_standby->safe_psql('postgres',
	"SELECT count(*) FROM pg_stat_wal_receiver");
is($walrcv_count, '0',
	"archive standby: still no WAL receiver after all tests");

$node_standby->stop;
$node_primary->stop;

done_testing();
