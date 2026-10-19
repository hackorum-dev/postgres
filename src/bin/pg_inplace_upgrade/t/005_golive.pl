# Copyright (c) 2026, PostgreSQL Global Development Group

# The golive command, from the new version running in upgrade mode: the point
# of no return of the upgrade.  Needs the old installation from
# PG_TEST_INPLACE_OLDINSTALL (see src/test/recovery/t/062_inplace_upgrade.pl).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
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

my ($newbindir) = run_command([ 'pg_config', '--bindir' ]);
chomp $newbindir;

my $node =
  PostgreSQL::Test::Cluster->new('node', install_path => $oldinstall);
$node->init(force_initdb => 1);
$node->start;
$node->safe_psql(
	'postgres', q{
	CREATE ROLE app LOGIN;
	CREATE TABLE t (a int);
	INSERT INTO t VALUES (1);
	GRANT SELECT ON t TO app;
});
my $old_version = $node->safe_psql('postgres', 'SHOW server_version_num');

my @pg_inplace_upgrade = (
	"$newbindir/pg_inplace_upgrade",
	'--pgdata' => $node->data_dir,
	'--old-bindir' => "$oldinstall/bin",
	'--new-bindir' => $newbindir,
	'--log' => $node->logfile);

# Nothing to take live while the old version runs.
command_fails_like(
	[ @pg_inplace_upgrade, 'golive' ],
	qr/has not been swapped to the new version/,
	'golive refused on the old version');

$node->command_ok([ @pg_inplace_upgrade, 'prepare' ], 'prepared');
$node->command_ok([ @pg_inplace_upgrade, 'swap' ], 'swapped');
$node->_update_pid(1);
$node->set_install_path(undef);
my $new_version = $node->safe_psql('postgres', 'SHOW server_version_num');
isnt($new_version, $old_version, 'new version runs');

# Go live.  The command reports the end of the go-live checkpoint record.
sub golive
{
	my ($test_name) = @_;
	my ($stdout, $stderr);

	my $result = IPC::Run::run [ @pg_inplace_upgrade, 'golive' ],
	  '>' => \$stdout,
	  '2>' => \$stderr;
	ok($result, "$test_name: exit code 0");
	note($stderr) if $stderr ne '';
	my ($lsn) = $stderr =~
	  m{the go-live checkpoint record ends at ([0-9A-F]+/[0-9A-F]+)$}m;
	ok(defined $lsn, "$test_name: reports an LSN");
	return $lsn;
}
my $golive_lsn = golive('went live');
is($node->safe_psql('postgres', 'SHOW server_version_num'),
	$new_version, 'new version still runs');
is( slurp_file($node->data_dir . '/PG_VERSION'),
	int($new_version / 10000) . "\n",
	'PG_VERSION names the new major version');
ok( !-e $node->data_dir . '/pg_inplace_upgrade.conf'
	  && -e $node->data_dir . '/pg_inplace_upgrade.conf.disabled'
	  && !-e $node->data_dir . '/pg_inplace_upgrade_hba.conf'
	  && -e $node->data_dir . '/pg_inplace_upgrade_hba.conf.disabled',
	'configuration and authentication files withdrawn');
command_like(
	[ @pg_inplace_upgrade, 'status' ],
	qr/^Upgrade configuration:\s+withdrawn$/m,
	'status shows the configuration withdrawn');
is($node->safe_psql('postgres', 'SHOW autovacuum'),
	'on', 'autovacuum is on again');
$node->connect_ok(
	$node->connstr('postgres') . ' user=app',
	'application connection admitted');
is( $node->safe_psql(
		'postgres',
		'SELECT count(*) FROM t',
		connstr => $node->connstr('postgres') . ' user=app'),
	'1',
	'application reads its data');

# Run again, the command reports the same go-live checkpoint.
is(golive('golive run again'), $golive_lsn, 'same LSN reported again');

# As if the command had been cut short after the go-live checkpoint: the
# files are back in place, and the server restarts.  Run again, the command
# still reports the same go-live checkpoint, and withdraws the files.
sub reinstate_upgrade_files
{
	foreach
	  my $file ('pg_inplace_upgrade.conf', 'pg_inplace_upgrade_hba.conf')
	{
		my $path = $node->data_dir . "/$file";
		rename "$path.disabled", $path
		  or die "could not rename $path.disabled: $!";
	}
}
reinstate_upgrade_files();
$node->restart;
is(golive('golive run again after a restart'),
	$golive_lsn, 'same LSN reported after a restart');
ok( !-e $node->data_dir . '/pg_inplace_upgrade.conf'
	  && !-e $node->data_dir . '/pg_inplace_upgrade_hba.conf',
	'configuration and authentication files withdrawn after a restart');
is($node->safe_psql('postgres', 'SHOW autovacuum'),
	'on', 'autovacuum is on again after a restart');

# With the server stopped, the command starts the new version first.
reinstate_upgrade_files();
$node->stop;
is(golive('golive run on a stopped server'),
	$golive_lsn, 'same LSN reported from a stopped server');
$node->_update_pid(1);
$node->connect_ok($node->connstr('postgres') . ' user=app',
	'application connection admitted after starting the server');

# There is no way back, with the server running or stopped.
command_fails_like(
	[ @pg_inplace_upgrade, 'rollback' ],
	qr/has gone live and cannot be rolled back/,
	'rollback refused after go-live');
$node->stop;
command_fails_like(
	[
		"$newbindir/postgres", '--inplace-upgrade-rollback',
		'-D' => $node->data_dir
	],
	qr/the in-place upgrade has gone live and cannot be rolled back/,
	'rollback mode refused after go-live');

# Without the marker, the control file still says that the upgrade has gone
# live.
my $golive = $node->data_dir . '/pg_inplace_upgrade_bundle/markers/golive';
rename $golive, "$golive.gone" or die "could not rename $golive: $!";
command_like(
	[ @pg_inplace_upgrade, 'status' ],
	qr/^Upgrade posture:\s+live$/m,
	'status shows the upgrade live without the go-live marker');
command_fails_like(
	[
		"$newbindir/postgres", '--inplace-upgrade-rollback',
		'-D' => $node->data_dir
	],
	qr/the in-place upgrade has gone live and cannot be rolled back/,
	'rollback mode refused by the control file after go-live');
rename "$golive.gone", $golive or die "could not rename $golive back: $!";
$node->start;
is($node->safe_psql('postgres', 'SHOW server_version_num'),
	$new_version, 'new version runs after a restart');

$node->stop;

done_testing();
