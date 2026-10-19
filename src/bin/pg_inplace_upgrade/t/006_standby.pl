# Copyright (c) 2026, PostgreSQL Global Development Group

# The commands on a standby: swap crosses it to the new version once the
# upgrade has gone live on the primary, rollback resumes its replay past the
# boundary, and the rest are refused.  Needs the old installation from
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

# The standby streams the new version's WAL from the start checkpoint on.
# Without an archive, only wal_keep_size keeps the start segment on the
# primary past the go-live checkpoint; the migrator refuses replication
# slots.
my $primary =
  PostgreSQL::Test::Cluster->new('primary', install_path => $oldinstall);
$primary->init(allows_streaming => 1, force_initdb => 1);
$primary->append_conf('postgresql.conf', "wal_keep_size = '1GB'");
$primary->start;
$primary->safe_psql(
	'postgres', q{
	CREATE TABLE t (a int);
	INSERT INTO t VALUES (1);
});
my $old_version = $primary->safe_psql('postgres', 'SHOW server_version_num');

$primary->backup('bkp');
my $standby =
  PostgreSQL::Test::Cluster->new('standby', install_path => $oldinstall);
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->start;
$primary->wait_for_catchup($standby);

sub pg_inplace_upgrade
{
	my ($node) = @_;
	return (
		"$newbindir/pg_inplace_upgrade",
		'--pgdata' => $node->data_dir,
		'--old-bindir' => "$oldinstall/bin",
		'--new-bindir' => $newbindir,
		'--log' => $node->logfile);
}
my @on_primary = pg_inplace_upgrade($primary);
my @on_standby = pg_inplace_upgrade($standby);

# First upgrade, rolled back.  The standby pauses at the boundary; the
# commands of the primary are refused on it, and so is swap until the
# upgrade has gone live on the primary: while the primary is stopped, and
# while it is in upgrade mode.
$primary->command_ok(
	[ @on_primary, '--standby' => $standby->connstr('postgres'), 'prepare' ],
	'prepared');
$primary->_update_pid(0);
is( $standby->safe_psql('postgres', 'SELECT pg_get_wal_replay_pause_state()'),
	'paused',
	'standby paused at the upgrade boundary');
command_fails_like(
	[ @on_standby, 'prepare' ],
	qr/is a standby/,
	'prepare refused on the standby');
command_fails_like(
	[ @on_standby, 'golive' ],
	qr/is a standby/,
	'golive refused on the standby');
command_fails_like(
	[ @on_standby, 'swap' ],
	qr/could not connect to the primary/,
	'swap refused on the standby while the primary is stopped');

$primary->command_ok([ @on_primary, 'swap' ], 'swapped the primary');
$primary->_update_pid(1);
command_fails_like(
	[ @on_standby, 'swap' ],
	qr/cannot start WAL sender during an in-place upgrade/,
	'swap refused on the standby while the primary is in upgrade mode');
is( $standby->safe_psql('postgres', 'SELECT pg_get_wal_replay_pause_state()'),
	'paused',
	'standby still paused at the upgrade boundary');

# Rolled back, the primary runs the old version again, and the standby
# resumes behind it.
$primary->command_ok([ @on_primary, 'rollback' ], 'rolled the primary back');
$primary->_update_pid(1);
is($primary->safe_psql('postgres', 'SHOW server_version_num'),
	$old_version, 'old version runs on the primary');
$primary->safe_psql('postgres', 'INSERT INTO t VALUES (2)');
$standby->command_ok([ @on_standby, 'rollback' ], 'rolled the standby back');
$primary->wait_for_catchup($standby);
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'2', 'standby follows the primary after the rollback');
$standby->command_ok([ @on_standby, 'rollback' ],
	'rollback on the standby run again');

# Second upgrade, taken live.  The standby crosses: the swap command stops
# the old version on it, starts the new one, and returns once the standby
# has replayed the go-live checkpoint.
$primary->command_ok(
	[ @on_primary, '--standby' => $standby->connstr('postgres'), 'prepare' ],
	'prepared again');
$primary->_update_pid(0);
$primary->command_ok([ @on_primary, 'swap' ], 'swapped the primary again');
$primary->_update_pid(1);
$primary->set_install_path(undef);
$primary->command_ok([ @on_primary, 'golive' ], 'took the upgrade live');
my $new_version = $primary->safe_psql('postgres', 'SHOW server_version_num');
isnt($new_version, $old_version, 'new version runs on the primary');

$standby->command_ok([ @on_standby, 'swap' ], 'crossed the standby');
$standby->set_install_path(undef);
$standby->_update_pid(1);
is($standby->safe_psql('postgres', 'SHOW server_version_num'),
	$new_version, 'new version runs on the standby');
is($standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'),
	't', 'standby is still a standby');
$primary->safe_psql('postgres', 'INSERT INTO t VALUES (3)');
$primary->wait_for_catchup($standby);
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'3', 'standby follows the primary on the new version');
command_like(
	[ @on_standby, 'status' ],
	qr/^Upgrade posture:\s+live$/m,
	'status shows the standby on the new version');
command_like(
	[ @on_standby, 'status' ],
	qr/^Consistency hold:\s+cleared$/m,
	'status shows the consistency hold cleared');

# Run again, swap finds the standby crossed; there is no way back.
$standby->command_ok([ @on_standby, 'swap' ],
	'swap on the standby run again');
command_fails_like(
	[ @on_standby, 'rollback' ],
	qr/cannot be rolled back/,
	'rollback refused on the crossed standby');

$standby->stop;
$primary->stop;

done_testing();
