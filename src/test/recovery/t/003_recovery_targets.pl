# Test for recovery targets: name, timestamp, XID
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 10;
use Time::HiRes qw(usleep);

# Create and test a standby from given backup, with a certain recovery target.
# Choose $until_lsn later than the transaction commit that causes the row
# count to reach $num_rows, yet not later than the recovery target.
sub test_recovery_standby
{
	my $test_name       = shift;
	my $node_name       = shift;
	my $node_master     = shift;
	my $recovery_params = shift;
	my $num_rows        = shift;
	my $until_lsn       = shift;

	my $node_standby = get_new_node($node_name);
	$node_standby->init_from_backup($node_master, 'my_backup',
		has_restoring => 1);

	foreach my $param_item (@$recovery_params)
	{
		$node_standby->append_conf('postgresql.conf', qq($param_item));
	}

	$node_standby->start;

	# Wait until standby has replayed enough data
	my $caughtup_query =
	  "SELECT '$until_lsn'::pg_lsn <= pg_last_wal_replay_lsn()";
	$node_standby->poll_query_until('postgres', $caughtup_query)
	  or die "Timed out while waiting for standby to catch up";

	# Create some content on master and check its presence in standby
	my $result =
	  $node_standby->safe_psql('postgres', "SELECT count(*) FROM tab_int");
	is($result, qq($num_rows), "check standby content for $test_name");

	# Stop standby node
	$node_standby->teardown_node;

	return;
}

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(has_archiving => 1, allows_streaming => 1);

# Start it
$node_master->start;

# Create data before taking the backup, aimed at testing
# recovery_target = 'immediate'
$node_master->safe_psql('postgres',
	"CREATE TABLE tab_int AS SELECT generate_series(1,1000) AS a");
my $lsn1 =
  $node_master->safe_psql('postgres', "SELECT pg_current_wal_lsn();");

# Take backup from which all operations will be run
$node_master->backup('my_backup');

# Insert some data with used as a replay reference, with a recovery
# target TXID.
$node_master->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(1001,2000))");
my $ret = $node_master->safe_psql('postgres',
	"SELECT pg_current_wal_lsn(), txid_current();");
my ($lsn2, $recovery_txid) = split /\|/, $ret;

# More data, with recovery target timestamp
$node_master->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(2001,3000))");
my $lsn3 =
  $node_master->safe_psql('postgres', "SELECT pg_current_wal_lsn();");
my $recovery_time = $node_master->safe_psql('postgres', "SELECT now()");

# Even more data, this time with a recovery target name
$node_master->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(3001,4000))");
my $recovery_name = "my_target";
my $lsn4 =
  $node_master->safe_psql('postgres', "SELECT pg_current_wal_lsn();");
$node_master->safe_psql('postgres',
	"SELECT pg_create_restore_point('$recovery_name');");

# And now for a recovery target LSN
$node_master->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(4001,5000))");
my $lsn5 = my $recovery_lsn =
  $node_master->safe_psql('postgres', "SELECT pg_current_wal_lsn()");

$node_master->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(5001,6000))");

# Force archiving of WAL file
$node_master->safe_psql('postgres', "SELECT pg_switch_wal()");

# Test recovery targets
my @recovery_params = ("recovery_target = 'immediate'");
test_recovery_standby('immediate target',
	'standby_1', $node_master, \@recovery_params, "1000", $lsn1);
@recovery_params = ("recovery_target_xid = '$recovery_txid'");
test_recovery_standby('XID', 'standby_2', $node_master, \@recovery_params,
	"2000", $lsn2);
@recovery_params = ("recovery_target_time = '$recovery_time'");
test_recovery_standby('time', 'standby_3', $node_master, \@recovery_params,
	"3000", $lsn3);
@recovery_params = ("recovery_target_name = '$recovery_name'");
test_recovery_standby('name', 'standby_4', $node_master, \@recovery_params,
	"4000", $lsn4);
@recovery_params = ("recovery_target_lsn = '$recovery_lsn'");
test_recovery_standby('LSN', 'standby_5', $node_master, \@recovery_params,
	"5000", $lsn5);

# Multiple targets
#
# Multiple conflicting settings are not allowed, but setting the same
# parameter multiple times or unsetting a parameter and setting a
# different one is allowed.

@recovery_params = (
	"recovery_target_name = '$recovery_name'",
	"recovery_target_name = ''",
	"recovery_target_time = '$recovery_time'");
test_recovery_standby('multiple overriding settings',
	'standby_6', $node_master, \@recovery_params, "3000", $lsn3);

my $node_standby = get_new_node('standby_7');
$node_standby->init_from_backup($node_master, 'my_backup',
	has_restoring => 1);
$node_standby->append_conf(
	'postgresql.conf', "recovery_target_name = '$recovery_name'
recovery_target_time = '$recovery_time'");

my $res = run_log(
	[
		'pg_ctl',               '-D', $node_standby->data_dir, '-l',
		$node_standby->logfile, 'start'
	]);
ok(!$res, 'invalid recovery startup fails');

my $logfile = slurp_file($node_standby->logfile());
ok($logfile =~ qr/multiple recovery targets specified/,
	'multiple conflicting settings');

# Check behavior when recovery ends before target is reached

$node_standby = get_new_node('standby_8');
$node_standby->init_from_backup($node_master, 'my_backup',
								has_restoring => 1, standby => 0);
$node_standby->append_conf('postgresql.conf',
						   "recovery_target_name = 'does_not_exist'");

run_log(['pg_ctl', '-D', $node_standby->data_dir,
		 '-l', $node_standby->logfile, 'start']);

# wait up to 10 seconds for postgres to terminate
foreach my $i (0..100)
{
	last if ! -f $node_standby->data_dir . '/postmaster.pid';
	usleep(100_000);
}
$logfile = slurp_file($node_standby->logfile());
ok($logfile =~ qr/FATAL:  recovery ended before configured recovery target was reached/,
	'recovery end before target reached is a fatal error');

# Corner case where targetted promotion happens on segment boundary
$node_standby = get_new_node('standby_9');
$node_standby->init_from_backup($node_master, 'my_backup',
								has_restoring => 1, has_streaming => 1);
$node_standby->start;
## make sure replication stays at the beginning of a segment
$node_master->safe_psql('postgres', "VACUUM; SELECT pg_switch_wal();");
my $catch_up_lsn =
  $node_master->safe_psql('postgres', "SELECT pg_current_wal_lsn();");
my $caughtup_query =
  "SELECT '$catch_up_lsn'::pg_lsn <= pg_last_wal_replay_lsn()";
$node_standby->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for standby to catch up";
## calculate the next segment boundary
my $result =
  $node_standby->safe_psql('postgres', "SHOW wal_segment_size");
die "unknown format of wal_segment_size: $result\n"
  if ($result !~ /^([0-9]+)MB$/);
my $segsize = $1 * 1024 * 1024;
$result =
  $node_standby->safe_psql('postgres', "SELECT pg_last_wal_replay_lsn()");
my ($fileno, $off) = split('/', $result);
my ($newoff, $newfileno) = (hex($off), hex($fileno));
my $newsegnum = int($newoff / $segsize) + 1;
## treat segment-overflow, avoiding over-32bit arithmetic, segsize is
## a power of 2 larger than 1MB.
if ($newsegnum * ($segsize >> 1) >= 0x80000000)
{
	$newfileno += ($newsegnum * ($segsize >> 1)) >> 31;
	$newsegnum %= (0x80000000 / ($segsize >> 1));
}
my $target = sprintf("%X/%08X", $newfileno, $newsegnum * $segsize);
## the standby stops just after the next segment boundary
$node_standby->stop;
$node_standby->append_conf('postgresql.conf', qq(
recovery_target_inclusive=no
recovery_target_lsn='$target'
recovery_target_action='promote'
));
$node_standby->start;
## do targetted promote
$node_master->safe_psql('postgres', "CREATE TABLE t(); DROP TABLE t;");
$node_master->safe_psql('postgres', "SELECT pg_switch_wal(); CHECKPOINT;");
$caughtup_query = "SELECT NOT pg_is_in_recovery()";
$node_standby->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for standby to promote";
pass("targetted promotion on segment bounary");
