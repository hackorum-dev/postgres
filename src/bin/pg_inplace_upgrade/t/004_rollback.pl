# Copyright (c) 2026, PostgreSQL Global Development Group

# The rollback command, from the new version running in upgrade mode, and its
# refusal once the upgrade goes live.  Needs the old installation from
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
$node->command_ok([ @pg_inplace_upgrade, 'prepare' ], 'prepared');
$node->command_ok([ @pg_inplace_upgrade, 'swap' ], 'swapped');
$node->_update_pid(1);
isnt($node->safe_psql('postgres', 'SHOW server_version_num'),
	$old_version, 'new version runs');

# Once the go-live marker is written, the upgrade cannot be rolled back, and
# the new version keeps running.
my $golive = $node->data_dir . '/pg_inplace_upgrade_bundle/markers/golive';
open my $fh, '>', $golive or die "could not create $golive: $!";
close $fh;
command_like(
	[ @pg_inplace_upgrade, 'status' ],
	qr/^Upgrade posture:\s+live$/m,
	'status shows the upgrade live once the go-live marker is written');
command_fails_like(
	[ @pg_inplace_upgrade, 'rollback' ],
	qr/has gone live and cannot be rolled back/,
	'rollback refused after go-live');
ok(-e $node->data_dir . '/postmaster.pid', 'new version still running');
unlink $golive or die "could not remove $golive: $!";

# From the running new version, the rollback command stops it, restores the
# old version's files, withdraws the configuration and authentication files,
# and starts the old version.  The restore reports to the server log.
my $offset = -s $node->logfile;
my ($rollback_stdout, $rollback_stderr);
ok( IPC::Run::run(
		[ @pg_inplace_upgrade, 'rollback' ],
		'>' => \$rollback_stdout,
		'2>' => \$rollback_stderr),
	'rolled back');
unlike($rollback_stderr, qr/LOG:/, 'no server log lines in the output');
like(
	substr(slurp_file($node->logfile), $offset),
	qr/LOG:  rolled back the in-place upgrade/,
	'restore reported in the server log');
$node->_update_pid(1);
is($node->safe_psql('postgres', 'SHOW server_version_num'),
	$old_version, 'old version runs');
ok(!-e $node->data_dir . '/pg_inplace_upgrade_bundle',
	'rollback bundle removed');
ok( !-e $node->data_dir . '/pg_inplace_upgrade.conf'
	  && -e $node->data_dir . '/pg_inplace_upgrade.conf.disabled'
	  && !-e $node->data_dir . '/pg_inplace_upgrade_hba.conf'
	  && -e $node->data_dir . '/pg_inplace_upgrade_hba.conf.disabled',
	'configuration and authentication files withdrawn');
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
command_like(
	[ @pg_inplace_upgrade, 'status' ],
	qr/^Upgrade configuration:\s+withdrawn$/m,
	'status shows the configuration withdrawn');

# Run again, the rollback command finds nothing left to do.
$node->command_ok([ @pg_inplace_upgrade, 'rollback' ], 'rollback run again');
is($node->safe_psql('postgres', 'SHOW server_version_num'),
	$old_version, 'old version still runs');

$node->stop;

done_testing();
