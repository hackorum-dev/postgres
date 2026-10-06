# Copyright (c) 2026, PostgreSQL Global Development Group

# The check and prepare commands against an old-version primary with a
# streaming standby.  The environment variable PG_TEST_INPLACE_OLDINSTALL
# points to the installation of the old version, a build of the commit
# before this tree was stamped with its major version; without it, the test
# is skipped.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Digest::SHA qw(sha256_hex);
use Test::More;

if (!defined $ENV{PG_TEST_INPLACE_OLDINSTALL})
{
	plan skip_all => 'PG_TEST_INPLACE_OLDINSTALL is not set';
}
if (!$use_unix_sockets)
{
	plan skip_all => 'pg_inplace_upgrade requires Unix-domain sockets';
}
my $oldinstall = $ENV{PG_TEST_INPLACE_OLDINSTALL};

# IPC::Run caches the path of a command however PATH changes, so qualify
# new-version commands too, not just those of old-version nodes.
my ($newbindir) = run_command([ 'pg_config', '--bindir' ]);
chomp $newbindir;

sub file_digest
{
	my ($path) = @_;
	return sha256_hex(slurp_file($path));
}

my $primary =
  PostgreSQL::Test::Cluster->new('primary', install_path => $oldinstall);
$primary->init(allows_streaming => 1, force_initdb => 1);
$primary->append_conf('postgresql.conf', 'max_prepared_transactions = 5');
$primary->start;
$primary->safe_psql(
	'postgres', q{
	CREATE ROLE app LOGIN;
	CREATE TABLE t (a int PRIMARY KEY);
	INSERT INTO t SELECT generate_series(1, 100);
	GRANT SELECT ON t TO app;
});
$primary->append_conf('pg_hba.conf', 'local all app trust');
$primary->reload;

$primary->backup('bkp');
my $standby =
  PostgreSQL::Test::Cluster->new('standby', install_path => $oldinstall);
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->append_conf('postgresql.conf', 'hot_standby_feedback = on');
$standby->start;
$primary->wait_for_catchup($standby);

my @pg_inplace_upgrade = (
	"$newbindir/pg_inplace_upgrade",
	'--pgdata' => $primary->data_dir,
	'--old-bindir' => "$oldinstall/bin",
	'--new-bindir' => $newbindir,
	'--standby' => $standby->connstr('postgres'));

# Fetch one line of the status command's output
sub status_field
{
	my ($field) = @_;

	my ($stdout, $stderr) = run_command([ @pg_inplace_upgrade, 'status' ]);
	die "$field not found in the status\n"
	  unless $stdout =~ /^\Q$field\E:\s*(.*)$/m;

	return $1;
}

# The standby reports its transaction horizon to the primary only every
# wal_receiver_status_interval, and until it has, rows deleted on the
# primary after the last report cannot be frozen.  Wait for the report to
# pass the transactions so far before freezing the catalogs.
sub wait_for_feedback
{
	my $xid = $primary->safe_psql('postgres', 'SELECT pg_current_xact_id()');
	$primary->wait_for_catchup($standby);
	$primary->poll_query_until('postgres',
		qq{SELECT backend_xmin::text::bigint > $xid FROM pg_stat_replication})
	  or die "timed out waiting for the standby's feedback";
	return;
}

# The configuration files the upgrade must leave alone
my %conf_digests = map { $_ => file_digest($primary->data_dir . "/$_") }
  qw(pg_hba.conf postgresql.conf);

$primary->command_checks_all([ @pg_inplace_upgrade, 'check' ],
	0, [qr/^$/], [qr/can be upgraded/], 'check passes');
command_like(
	[ @pg_inplace_upgrade, 'status' ],
	qr/^Upgrade posture:\s+not prepared$/m,
	'status before prepare');

# A cluster of the new version that no upgrade touched.  Its control file
# has the layout of the new version, and is that version's.
my $fresh = PostgreSQL::Test::Cluster->new('fresh');
$fresh->init(force_initdb => 1);
command_like(
	[
		"$newbindir/pg_inplace_upgrade",
		'--pgdata' => $fresh->data_dir,
		'--old-bindir' => "$oldinstall/bin",
		'--new-bindir' => $newbindir,
		'status'
	],
	qr/^Owner:\s+new version\n.*^Upgrade posture:\s+live$/ms,
	'status of a cluster of the new version');

# check refuses each condition the migration would refuse after the swap
sub check_refuses
{
	my ($name, $setup, $teardown, $expected) = @_;
	$primary->safe_psql('postgres', $setup);
	$primary->command_fails_like([ @pg_inplace_upgrade, 'check' ],
		$expected, "check refuses $name");
	$primary->safe_psql('postgres', $teardown);
	return;
}
check_refuses(
	'a database that allows no connections',
	q{CREATE DATABASE noconn; ALTER DATABASE noconn ALLOW_CONNECTIONS false},
	q{DROP DATABASE noconn},
	qr/database "noconn" does not allow connections/);
check_refuses(
	'an invalid database',
	q{CREATE DATABASE inv; UPDATE pg_database SET datconnlimit = -2 WHERE datname = 'inv'},
	q{UPDATE pg_database SET datconnlimit = -1 WHERE datname = 'inv'; DROP DATABASE inv},
	qr/database "inv" is invalid/);
check_refuses(
	'a prepared transaction',
	q{BEGIN; INSERT INTO t VALUES (0); PREPARE TRANSACTION 'p1'},
	q{ROLLBACK PREPARED 'p1'},
	qr/1 prepared transaction/);
check_refuses(
	'a replication slot',
	q{SELECT pg_create_physical_replication_slot('s')},
	q{SELECT pg_drop_replication_slot('s')},
	qr/replication slot "s" exists/);
$primary->safe_psql('postgres',
	q{SELECT pg_create_physical_replication_slot('tmp', false, true)});
$primary->command_ok([ @pg_inplace_upgrade, 'check' ],
	'check ignores a temporary replication slot');

$primary->append_conf('postgresql.conf', 'track_commit_timestamp = on');
$primary->restart;
$primary->wait_for_catchup($standby);
$primary->command_fails_like(
	[ @pg_inplace_upgrade, 'check' ],
	qr/track_commit_timestamp is on/,
	'check refuses track_commit_timestamp');
$primary->adjust_conf('postgresql.conf', 'track_commit_timestamp', 'off');
$primary->restart;
$primary->wait_for_catchup($standby);
%conf_digests = map { $_ => file_digest($primary->data_dir . "/$_") }
  qw(pg_hba.conf postgresql.conf);
my $pg_hba_before = slurp_file($primary->data_dir . '/pg_hba.conf');

# prepare refuses a database that allows no connections before it changes
# anything
$primary->safe_psql('postgres',
	q{CREATE DATABASE noconn; ALTER DATABASE noconn ALLOW_CONNECTIONS false});
$primary->command_fails_like(
	[ @pg_inplace_upgrade, 'prepare' ],
	qr/database "noconn" does not allow connections/,
	'prepare refuses a database that allows no connections');
ok(!-e $primary->data_dir . '/pg_inplace_upgrade.conf',
	'prepare wrote nothing before refusing');
$primary->safe_psql('postgres', q{DROP DATABASE noconn});

# An application session, open when the upgrade begins
my $app = $primary->background_psql('postgres',
	connstr => $primary->connstr('postgres') . ' user=app');
$app->query_safe('SELECT count(*) FROM t');

# A condition that appears after the catalogs are frozen is refused before
# the boundary is requested.
$primary->safe_psql('postgres',
	q{SELECT pg_create_physical_replication_slot('late')});
wait_for_feedback();
$primary->command_fails_like(
	[ @pg_inplace_upgrade, 'prepare' ],
	qr/replication slot "late" exists/,
	'prepare refuses a replication slot after the freeze');
is( $primary->safe_psql('postgres', 'SELECT pg_upgrade_boundary_requested()'),
	'f',
	'no boundary requested after the refusal');
$primary->safe_psql('postgres', q{SELECT pg_drop_replication_slot('late')});

# The application session was terminated, before the refusal
is( $primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_stat_activity WHERE usename = 'app'"),
	'0',
	'application session terminated');
$app->quit;

# A transaction on the standby holds the transaction horizon back through
# hot_standby_feedback, so the catalog rows the DDL below leaves behind
# cannot be frozen.  prepare reports that.
my $held = $standby->background_psql('postgres');
$held->query_safe(
	'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t');
$primary->poll_query_until('postgres',
	q{SELECT backend_xmin IS NOT NULL FROM pg_stat_replication})
  or die "timed out waiting for the standby's feedback";
my $held_xmin = $primary->safe_psql('postgres',
	q{SELECT backend_xmin FROM pg_stat_replication});
$primary->safe_psql('postgres', q{CREATE TABLE t2 (a int); DROP TABLE t2});

$primary->command_checks_all(
	[ @pg_inplace_upgrade, 'prepare' ],
	1,
	[qr/^$/],
	[
		qr/pages of "pg_class" are frozen/,
		qr/run "pg_inplace_upgrade prepare" again/,
	],
	'prepare fails when the catalogs cannot be frozen');
is( $primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_stat_replication WHERE state = 'streaming'"),
	'1',
	'standby still streams after the failed prepare');
command_like(
	[ @pg_inplace_upgrade, 'status' ],
	qr/^Upgrade posture:\s+prepare in progress$/m,
	'status after the failed prepare');

# Once the standby's transaction ends and its feedback moves on, prepare
# succeeds.
$held->query_safe('COMMIT');
$held->quit;
$primary->poll_query_until(
	'postgres',
	qq{SELECT backend_xmin IS NULL OR backend_xmin::text::bigint > $held_xmin
	FROM pg_stat_replication}
) or die "timed out waiting for the standby's feedback to move on";

$primary->command_ok([ @pg_inplace_upgrade, 'prepare' ], 'prepare succeeds');

is( file_digest($primary->data_dir . '/postgresql.conf'),
	$conf_digests{'postgresql.conf'},
	'postgresql.conf untouched after prepare');
is( slurp_file($primary->data_dir . '/pg_hba.conf'),
	'include_if_exists "'
	  . $primary->data_dir
	  . '/pg_inplace_upgrade_hba.conf"' . "\n"
	  . $pg_hba_before,
	'pg_hba.conf gained the include line at the top, and nothing else');
is(status_field('Cluster state'), 'shut down', 'primary shut down cleanly');
my ($boundary_lsn, $upgrade_flag) =
  status_field('Latest checkpoint') =~ /^(\S+) \(upgrade flag (\S+)\)$/;
is($upgrade_flag, 'boundary',
	'primary shutdown checkpoint is marked as a boundary');
is($standby->safe_psql('postgres', 'SELECT pg_upgrade_boundary_lsn()'),
	$boundary_lsn, 'standby paused at the primary boundary');
ok(!-e $primary->data_dir . '/postmaster.pid', 'primary is stopped');
like(
	slurp_file($primary->data_dir . '/pg_inplace_upgrade.conf'),
	qr/^pg_inplace_upgrade\.configured = on\nautovacuum = off\n/m,
	'configuration file turns autovacuum off');
my $hba = slurp_file($primary->data_dir . '/pg_inplace_upgrade_hba.conf');
unlike($hba, qr/^local\tall\tall\ttrust/m,
	'authentication file does not admit everyone');
like(
	$hba,
	qr/^local\tall\t"[^"]+"\tpeer$/m,
	'authentication file admits the role running the upgrade');
like(
	$hba,
	qr/^local\tall\tall\treject\nhost\tall\tall\tall\treject\n\z/m,
	'authentication file rejects everyone else, last');
like(
	slurp_file($primary->data_dir . '/postgresql.auto.conf'),
	qr/^include_if_exists 'pg_inplace_upgrade\.conf'$/m,
	'postgresql.auto.conf includes the configuration file');
command_like(
	[ @pg_inplace_upgrade, 'status' ],
	qr/^Upgrade posture:\s+at an upgrade boundary$/m,
	'status after prepare');

# The authentication rules are in effect on the old version as long as the
# files stay: start it once more and let it refuse an application.  prepare
# stopped the node behind the test harness's back.
$primary->_update_pid(0);
$primary->start;
is( $primary->safe_psql(
		'postgres',
		'SELECT count(*) FROM pg_hba_file_rules WHERE error IS NOT NULL'),
	'0',
	'authentication rules load without error');
$primary->connect_fails(
	$primary->connstr('postgres') . ' user=app',
	'application connection refused by the authentication file',
	expected_stderr => qr/pg_hba\.conf rejects connection/);
is($primary->safe_psql('postgres', 'SHOW autovacuum'),
	'off', 'autovacuum is off while the files stay');
$primary->stop;

done_testing();
