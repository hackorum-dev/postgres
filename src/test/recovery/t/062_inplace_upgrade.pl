# Copyright (c) 2026, PostgreSQL Global Development Group

# Test an in-place upgrade of a primary and its streaming standby.
#
# An in-place upgrade crosses a major version boundary, so this test needs
# two installations: the current build is the new version, and the
# environment variable PG_TEST_INPLACE_OLDINSTALL points to the installation
# path of the old one.  Without it, the test is skipped.
#
# The old installation is a build of the commit before this tree was
# stamped with its major version, so that the two differ by one major
# version the way a real upgrade does.  With meson, for example:
#
#   git worktree add ../pg-old <commit before the version stamp>
#   meson setup ../pg-old/build --prefix=$HOME/pg-old
#   ninja -C ../pg-old/build install
#   meson test --suite setup
#   PG_TEST_INPLACE_OLDINSTALL=$HOME/pg-old meson test recovery/062_inplace_upgrade
#
# Tools reading on-disk formats, pg_waldump and pg_controldata included,
# must come from the installation matching the files they read.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use PostgreSQL::Version;
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

# Fetch one line of the status command's output for a node
sub status_field
{
	my ($node, $field) = @_;

	my ($stdout, $stderr) = run_command(
		[
			"$newbindir/pg_inplace_upgrade",
			'--pgdata' => $node->data_dir,
			'--old-bindir' => "$oldinstall/bin",
			'--new-bindir' => $newbindir,
			'status'
		]);
	die "$field not found in the status\n"
	  unless $stdout =~ /^\Q$field\E:\s*(.*)$/m;

	return $1;
}

# The location and the upgrade flag of a node's latest checkpoint
sub checkpoint_location
{
	my ($node) = @_;
	return (status_field($node, 'Latest checkpoint') =~ /^(\S+) /)[0];
}

sub checkpoint_flag
{
	my ($node) = @_;
	return (
		status_field($node, 'Latest checkpoint') =~ /\(upgrade flag (\S+)\)$/)
	  [0];
}

# The initdb template belongs to the current build, so force initdb to get
# an old-version cluster.
my $primary =
  PostgreSQL::Test::Cluster->new('primary', install_path => $oldinstall);
$primary->init(allows_streaming => 1, force_initdb => 1);

# Check the two installations before relying on them: one major version
# apart.
my ($new_version) = run_command([ "$newbindir/pg_config", '--version' ]);
$new_version = PostgreSQL::Version->new($new_version);
is( $primary->pg_version->major,
	$new_version->major - 1,
	'old installation is one major version behind the new one');

# XXX: not for commit.  The new version runs on the old version's catalogs
# until it has migrated them, and the migration copies their rows unchanged,
# so every catalog row this tree declares must already be in the old
# installation.  Comparing the bootstrap data, apart from the version on its
# first line, catches an old installation that was not rebuilt after the
# commits before the version stamp changed, and a later commit that touches
# the catalogs.
my ($old_sharedir) =
  run_command([ "$oldinstall/bin/pg_config", '--sharedir' ]);
my ($new_sharedir) = run_command([ "$newbindir/pg_config", '--sharedir' ]);
chomp($old_sharedir, $new_sharedir);
my $old_bki = slurp_file("$old_sharedir/postgres.bki");
my $new_bki = slurp_file("$new_sharedir/postgres.bki");
s/\A[^\n]*\n// for ($old_bki, $new_bki);
ok($old_bki eq $new_bki, 'old installation has the catalogs of this build')
  or die "catalogs of the old installation differ; rebuild it\n";

$primary->start;
$primary->safe_psql(
	'postgres', q{
	CREATE ROLE app LOGIN;
	CREATE TABLE t (a int PRIMARY KEY, b text);
	INSERT INTO t SELECT g, md5(g::text) FROM generate_series(1, 1000) g;
	ANALYZE t;
	GRANT SELECT ON t TO app;
});

$primary->backup('bkp');
my $standby =
  PostgreSQL::Test::Cluster->new('standby', install_path => $oldinstall);
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->start;
$primary->wait_for_catchup($standby);
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'1000', 'standby has the test data');

# An application session, open when the upgrade begins
my $app = $primary->background_psql('postgres',
	connstr => $primary->connstr('postgres') . ' user=app');
$app->query_safe('SELECT count(*) FROM t');

# Still on the old version, prepare the primary for the upgrade: refuse
# normal connections, terminate application backends, freeze the catalogs,
# then shut down at an upgrade boundary and wait for the standby to pause
# there.  Walsenders keep streaming until the shutdown, since the standby
# needs all WAL through the boundary.
my @pg_inplace_upgrade = (
	"$newbindir/pg_inplace_upgrade",
	'--pgdata' => $primary->data_dir,
	'--old-bindir' => "$oldinstall/bin",
	'--new-bindir' => $newbindir);
$primary->command_ok(
	[
		@pg_inplace_upgrade,
		'--standby' => $standby->connstr('postgres'),
		'prepare'
	],
	'prepared the primary');
$app->quit;
# prepare stopped the primary with its own pg_ctl.
$primary->_update_pid(0);
is(status_field($primary, 'Cluster state'),
	'shut down', 'primary shut down cleanly');
is(checkpoint_flag($primary),
	'boundary', 'primary shutdown checkpoint is marked as a boundary');
my $boundary_lsn = checkpoint_location($primary);

# The standby is paused at the boundary, which is the primary's shutdown
# checkpoint.
is( $standby->safe_psql('postgres', 'SELECT pg_get_wal_replay_pause_state()'),
	'paused',
	'standby paused at the upgrade boundary');
is($standby->safe_psql('postgres', 'SELECT pg_upgrade_boundary_lsn()'),
	$boundary_lsn, 'standby boundary is the primary shutdown checkpoint');
done_testing();
