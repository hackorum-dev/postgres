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
use Digest::SHA qw(sha256_hex);
use File::Find;
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

# Fetch one field from the control file, read by the node's own version, for
# what the status command does not show
sub control_field
{
	my ($node, $field) = @_;

	my ($stdout, $stderr) = run_command(
		[ $node->installed_command('pg_controldata'), $node->data_dir ]);
	die "$field not found in control file\n"
	  unless $stdout =~ /^\Q$field\E:\s*(.*)$/m;

	return $1;
}

# The initdb template belongs to the current build, so force initdb to get
# an old-version cluster.
my $primary =
  PostgreSQL::Test::Cluster->new('primary', install_path => $oldinstall);
$primary->init(allows_streaming => 1, force_initdb => 1);

# Archive with a command that succeeds without doing anything, so that we
# can test that the server suppresses archiving in upgrade mode
$primary->append_conf(
	'postgresql.conf', q{
archive_mode = on
archive_command = 'true'
});

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
# Digest of a file, for comparing copies of binary files
sub file_digest
{
	my ($path) = @_;
	return sha256_hex(slurp_file($path));
}

# The files the takeover replaces, as the old version left them
my $old_control_digest =
  file_digest($primary->data_dir . '/global/pg_control');
my $old_stats_digest =
  file_digest($primary->data_dir . '/pg_stat/pgstat.stat');

# The relation map files, which the migration saves before it changes them
my %old_relmap_digests;
{
	my $datadir = $primary->data_dir;
	foreach my $map (glob("$datadir/{global,base/*}/pg_filenode.map"))
	{
		my ($dir) = $map =~ m{^\Q$datadir\E/(.*)/pg_filenode\.map$};
		$old_relmap_digests{$dir} = file_digest($map);
	}
}

# An old-version cluster that was not shut down at an upgrade boundary is
# refused by the new version as any other incompatible cluster, untouched.
my $plain =
  PostgreSQL::Test::Cluster->new('plain', install_path => $oldinstall);
$plain->init(force_initdb => 1);
$plain->start;
$plain->stop;
$plain->set_install_path(undef);
$plain->start(fail_ok => 1);
like(
	slurp_file($plain->logfile),
	qr/database files are incompatible with server/,
	'new version refuses an old-version cluster not at an upgrade boundary');

# Everything in a node's data directory, by path, with the digest of each
# file.  Every start of either version rewrites postmaster.opts.
sub datadir_contents
{
	my ($node) = @_;
	my $datadir = $node->data_dir;
	my %contents;

	find(
		{
			no_chdir => 1,
			wanted => sub {
				my $path = substr($File::Find::name, length($datadir));
				return if $path eq '/postmaster.opts';
				$contents{$path} =
				  -d $File::Find::name ? 'd' : file_digest($File::Find::name);
			}
		},
		$datadir);
	return \%contents;
}

# The new version refuses to take over a cluster that ran with commit
# timestamps, before it changes anything in the data directory, so the old
# version starts on it again as usual.
my $commit_ts =
  PostgreSQL::Test::Cluster->new('commit_ts', install_path => $oldinstall);
$commit_ts->init(force_initdb => 1);
$commit_ts->append_conf('postgresql.conf', 'track_commit_timestamp = on');
$commit_ts->start;
$commit_ts->safe_psql('postgres', 'SELECT pg_request_upgrade_boundary()');
$commit_ts->stop;
my $commit_ts_contents = datadir_contents($commit_ts);

$commit_ts->set_install_path(undef);
ok(!$commit_ts->start(fail_ok => 1),
	'new version refuses the takeover with track_commit_timestamp');
like(
	slurp_file($commit_ts->logfile),
	qr/FATAL:  cannot take over a database cluster for an in-place upgrade while "track_commit_timestamp" is enabled/,
	'refusal names the setting');
is_deeply(datadir_contents($commit_ts),
	$commit_ts_contents, 'refused data directory untouched');

$commit_ts->set_install_path($oldinstall);
ok($commit_ts->start(fail_ok => 1),
	'old version starts on the refused cluster again')
  and $commit_ts->stop;

# The server migrates the catalogs by itself when it comes up in upgrade
# mode, and the migration refuses a cluster it cannot handle before it
# writes anything.  For each such condition, shut an old-version cluster
# down at an upgrade boundary with the condition present and start the new
# version on it.  The server shuts down again, and the reason is in the
# server log and in a file next to what the takeover saved.
my $refused =
  PostgreSQL::Test::Cluster->new('refused', install_path => $oldinstall);
$refused->init(force_initdb => 1);
$refused->append_conf('postgresql.conf',
	"autovacuum = off\nmax_prepared_transactions = 5");
$refused->start;
$refused->safe_psql(
	'postgres', q{CREATE TABLE p (a int);
	BEGIN;
	INSERT INTO p VALUES (1);
	PREPARE TRANSACTION 'p1';});
$refused->safe_psql('postgres', 'SELECT pg_request_upgrade_boundary()');
$refused->stop;
$refused->set_install_path(undef);
my $refused_offset = -s $refused->logfile;
ok(!$refused->start(fail_ok => 1), 'refused: start fails');
ok(!-e $refused->data_dir . '/postmaster.pid', 'refused: server is down');

my $refusal =
  qr/cannot migrate the catalogs while prepared transactions exist/;
my $refused_log = substr(slurp_file($refused->logfile), $refused_offset);
like($refused_log, $refusal,
	'refused: refusal names the reason in the server log');
like(
	$refused_log,
	qr/in-place upgrade migrator \(PID \d+\) exited with exit code 1/,
	'refused: migrator exited with an error');
like(
	$refused_log,
	qr/shutting down due to in-place upgrade migrator failure/,
	'refused: server shut down because of the refusal');
unlike(
	$refused_log,
	qr/database system is ready to accept connections/,
	'refused: server never reported ready');
my $refused_stash = $refused->data_dir . '/pg_inplace_upgrade_bundle';
like(slurp_file("$refused_stash/migrate_error"),
	qr/^55000\n$refusal\n\z/,
	'refused: refusal names the SQLSTATE and the reason in a file');
opendir(my $refused_dh, $refused_stash)
  or die "could not open $refused_stash: $!";
my @kept = sort grep { !/^\./ } readdir($refused_dh);
closedir($refused_dh);
is_deeply(
	\@kept,
	[qw(created_files migrate_error pg_control pgstat.stat)],
	'refused: refusal saved nothing and recorded no step');

# Swap the primary's binaries and start it.  The new version's startup
# process takes over the cluster and comes up in upgrade mode.  Let the
# authentication file that prepare wrote admit the application role again,
# before its rejecting rules, so that it is the server itself that refuses it.
my $hba_file = $primary->data_dir . '/pg_inplace_upgrade_hba.conf';
my $hba = slurp_file($hba_file);
$hba =~ s/^local\tall\tall\treject$/local all app trust\n$&/m
  or die "no rejecting rule in $hba_file";
open my $fh, '>', $hba_file or die "cannot write $hba_file: $!";
print $fh $hba;
close $fh;
$primary->set_install_path(undef);

# Log when the postmaster starts a background worker, to see when the
# logical replication launcher starts.
$primary->append_conf('postgresql.conf',
	"log_min_messages = 'warning,postmaster:debug1'");

# The server reports ready only once the migrator is done, so the swap
# returns only then.
my $log_offset = -s $primary->logfile;
$primary->command_ok(
	[ @pg_inplace_upgrade, '--log' => $primary->logfile, 'swap' ],
	'swapped the primary to the new version');
$primary->_update_pid(1);
like(
	slurp_file($primary->logfile),
	qr/took over the database cluster for an in-place upgrade/,
	'primary taken over at startup');
is(status_field($primary, 'Upgrade posture'),
	'migrated', 'primary is in upgrade mode');
is(checkpoint_flag($primary),
	'start', 'primary started from the start checkpoint');
my $start_lsn = checkpoint_location($primary);
is( slurp_file($primary->data_dir . '/PG_VERSION'),
	($new_version->major - 1) . "\n",
	'PG_VERSION still names the old major version');
cmp_ok($start_lsn, 'gt', $boundary_lsn,
	'start checkpoint lies past the boundary');

# The next transaction ID moved up to a pg_xact segment boundary
my $next_xid = control_field($primary, "Latest checkpoint's NextXID");
my $xacts_per_segment =
  control_field($primary, 'Database block size') * 4 *
  control_field($primary, 'Pages per SLRU segment');
$next_xid =~ s/^\d+://;
is($next_xid % $xacts_per_segment,
	0, 'next transaction ID is at a pg_xact segment boundary');

# What the takeover replaced is kept, and what it created is listed
my $stash = $primary->data_dir . '/pg_inplace_upgrade_bundle';
is(file_digest("$stash/pg_control"),
	$old_control_digest, 'old control file kept');
is(file_digest("$stash/pgstat.stat"),
	$old_stats_digest, 'old statistics file kept');
like(slurp_file("$stash/created_files"),
	qr/^pg_wal\/[0-9A-F]{24}\n$/,
	'created files list names the new WAL segment');

# The start checkpoint is new-version WAL, and the boundary segment is
# untouched.
command_like(
	[
		"$newbindir/pg_waldump",
		'--path' => $primary->data_dir . '/pg_wal',
		'--start' => $start_lsn,
		'--limit' => 1
	],
	qr/CHECKPOINT_SHUTDOWN .*upgrade flag start/,
	'new pg_waldump shows the start checkpoint');
my @waldump_args = (
	'--path' => $primary->data_dir . '/pg_wal',
	'--start' => $boundary_lsn,
	'--limit' => 1);
command_like(
	[ $oldinstall . '/bin/pg_waldump', @waldump_args ],
	qr/CHECKPOINT_SHUTDOWN .*upgrade flag boundary/,
	'old pg_waldump still shows the boundary checkpoint');

my $checkpoint_before_migration = $start_lsn;
ok(-e $primary->data_dir . '/pg_inplace_upgrade_bundle/markers/migrated',
	'start returned with the catalogs migrated');

# The server reported ready, and started what reads the catalogs, only
# after the migration.  What it saved for a rollback matches the old
# cluster.
my $log = substr(slurp_file($primary->logfile), $log_offset);
like(
	$log,
	qr/LOG:  the catalogs are migrated.*LOG:  database system is ready to accept connections/s,
	'ready reported after the migration');
unlike(
	$log,
	qr/starting background worker process "logical replication launcher".*the catalogs are migrated/s,
	'logical replication launcher not started before the migration');

# Upgrade mode suppresses checkpoints, so the latest checkpoint is still
# the one before the migration.
my $last_checkpoint_lsn = checkpoint_location($primary);
is($last_checkpoint_lsn, $checkpoint_before_migration,
	'no checkpoint since the migration');
my $databases = $primary->safe_psql('postgres',
	q{SELECT oid || ' base/' || oid FROM pg_database ORDER BY oid});

is(file_digest("$stash/pg_control"),
	$old_control_digest, 'old control file still kept');
is( file_digest("$stash/pg_filenode.map.global"),
	$old_relmap_digests{global},
	'shared relation map file kept');
foreach my $database (split /\n/, $databases)
{
	my ($dboid, $dbpath) = split / /, $database;
	is( file_digest("$stash/pg_filenode.map.$dboid"),
		$old_relmap_digests{$dbpath},
		"relation map file of database $dboid kept");
}
is(join("\n", sort split(/\n/, slurp_file("$stash/databases"))),
	$databases, 'databases listed with their paths');

# Only superusers get in
$primary->connect_fails(
	$primary->connstr('postgres') . ' user=app',
	'application connection refused in upgrade mode',
	expected_stderr =>
	  qr/must be superuser to connect during an in-place upgrade/);
$primary->connect_fails(
	$primary->connstr('postgres') . ' replication=true',
	'replication connection refused in upgrade mode',
	expected_stderr => qr/cannot start WAL sender during an in-place upgrade/
);
is($primary->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'1000', 'superuser connection admitted, data readable');

# Archiving is fenced: no archiver runs, and switching WAL segments creates
# no .ready file.
my $switched_segment =
  $primary->safe_psql('postgres', 'SELECT pg_walfile_name(pg_switch_wal())');
my $archivers = $primary->safe_psql('postgres',
	"SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'archiver'");
ok( $archivers eq '0'
	  && !-e $primary->data_dir
	  . "/pg_wal/archive_status/$switched_segment.ready",
	'no archiver runs and no .ready file is created in upgrade mode');

# The checkpointer is suppressed: an explicit request fails.
my ($ret, $out, $err) = $primary->psql('postgres', 'CHECKPOINT');
like($err, qr/checkpoint request failed/, 'CHECKPOINT fails in upgrade mode');

# The old version refuses the taken-over cluster; the new version comes back
# up in upgrade mode.  Its shutdown checkpoint removes no WAL, so the old
# version still finds the boundary after a rollback.
my $boundary_segment =
  $primary->safe_psql('postgres', "SELECT pg_walfile_name('$boundary_lsn')");
opendir(my $dh, $primary->data_dir . '/pg_wal')
  or die "could not open pg_wal: $!";
my @segments_before = sort grep { /^[0-9A-F]{24}$/ } readdir($dh);
closedir($dh);
ok( grep({ $_ eq $boundary_segment } @segments_before),
	'boundary segment present before the shutdown');
$primary->stop;
my @segments_missing =
  grep { !-e $primary->data_dir . "/pg_wal/$_" } @segments_before;
is_deeply(\@segments_missing, [],
	'shutdown in upgrade mode removed no WAL segment');
is(status_field($primary, 'Upgrade posture'),
	'migrated', 'upgrade mode persists across a shutdown');
$primary->set_install_path($oldinstall);
ok(!$primary->start(fail_ok => 1),
	'old version refuses the taken-over cluster');
like(
	slurp_file($primary->logfile),
	qr/database files are incompatible with server/,
	'refusal is the version mismatch');
$primary->set_install_path(undef);
my $offset = -s $primary->logfile;
$primary->start;
is(status_field($primary, 'Upgrade posture'),
	'migrated', 'primary restarted in upgrade mode');
is($primary->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'1000', 'superuser connection admitted after the restart');
unlike(
	substr(slurp_file($primary->logfile), $offset),
	qr/in-place upgrade migrator|checking for prepared transactions/,
	'no second migration once the catalogs are migrated');

done_testing();
