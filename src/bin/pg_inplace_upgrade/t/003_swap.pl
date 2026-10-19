# Copyright (c) 2026, PostgreSQL Global Development Group

# The swap command's refusals, and status around a refused migration.  Needs
# the old installation from PG_TEST_INPLACE_OLDINSTALL (see
# src/test/recovery/t/062_inplace_upgrade.pl).

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

sub pg_inplace_upgrade
{
	my ($node) = @_;
	return (
		"$newbindir/pg_inplace_upgrade",
		'--pgdata' => $node->data_dir,
		'--old-bindir' => "$oldinstall/bin",
		'--new-bindir' => $newbindir);
}

sub old_node
{
	my ($name, $conf) = @_;
	my $node =
	  PostgreSQL::Test::Cluster->new($name, install_path => $oldinstall);
	$node->init(force_initdb => 1);
	$node->append_conf('postgresql.conf', "autovacuum = off\n$conf");
	$node->start;
	return $node;
}

# A running cluster, and one stopped without a boundary, are refused
my $plain = old_node('plain', '');
command_fails_like([ pg_inplace_upgrade($plain), 'swap' ],
	qr/is running/, 'swap refuses a running cluster');
$plain->stop;
command_fails_like(
	[ pg_inplace_upgrade($plain), 'swap' ],
	qr/is not shut down at an upgrade boundary/,
	'swap refuses a cluster not shut down at an upgrade boundary');
command_like(
	[ pg_inplace_upgrade($plain), 'status' ],
	qr/^Upgrade posture:\s+not prepared$/m,
	'status of a cluster not shut down at an upgrade boundary');

# A cluster the migration refuses: status reports the reason from the file
# the migrator leaves, with the server down.
my $refused = old_node('refused', 'max_prepared_transactions = 5');
$refused->safe_psql(
	'postgres', q{
	CREATE TABLE p (a int);
	BEGIN;
	INSERT INTO p VALUES (1);
	PREPARE TRANSACTION 'p1';
	SELECT pg_request_upgrade_boundary();
});
$refused->stop;
command_like(
	[ pg_inplace_upgrade($refused), 'status' ],
	qr/^Upgrade posture:\s+at an upgrade boundary$/m,
	'status before the swap');
my $refusal =
  qr/cannot migrate the catalogs while prepared transactions exist/;
command_checks_all(
	[ pg_inplace_upgrade($refused), '--log' => $refused->logfile, 'swap' ],
	1,
	[qr/^$/],
	[ qr/did not start/, qr/See ".*pg_inplace_upgrade status"/ ],
	'swap fails when the migration is refused');
ok(!-e $refused->data_dir . '/postmaster.pid', 'server is down');
command_checks_all(
	[ pg_inplace_upgrade($refused), 'status' ],
	0,
	[
		qr/^Upgrade posture:\s+migration failed$/m,
		qr/^Migration error:\n\s+55000\n\s+$refusal/m
	],
	[qr/^$/],
	'status after the refused migration');
command_checks_all(
	[ pg_inplace_upgrade($refused), 'swap' ],
	1,
	[qr/^$/],
	[
		qr/already been taken over by the new version/,
		qr/See ".*pg_inplace_upgrade status"/
	],
	'swap run again refuses a cluster already taken over');

done_testing();
