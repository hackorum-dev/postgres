# Copyright (c) 2026, PostgreSQL Global Development Group

# Where a cluster stands in an in-place upgrade, its posture, as the status
# command shows it, on data directories forged from a real one: PG_VERSION
# and the control file edited, no server running.  Needs the old
# installation from PG_TEST_INPLACE_OLDINSTALL (see t/002_prepare.pl).

use strict;
use warnings FATAL => 'all';
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::RecursiveCopy;
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

my $tempdir = PostgreSQL::Test::Utils::tempdir;

sub pg_inplace_upgrade
{
	my ($datadir) = @_;
	return (
		"$newbindir/pg_inplace_upgrade",
		'--pgdata' => $datadir,
		'--old-bindir' => "$oldinstall/bin",
		'--new-bindir' => $newbindir);
}

# A copy of a data directory to forge
sub forge
{
	my ($from, $name) = @_;
	my $datadir = "$tempdir/$name";

	PostgreSQL::Test::RecursiveCopy::copypath($from, $datadir);
	chmod 0700, $datadir;
	return $datadir;
}

sub write_file
{
	my ($path, $offset, $bytes) = @_;
	open my $fh, '+<', $path or die "could not open $path: $!";
	binmode $fh;
	seek $fh, $offset, 0 or die "could not seek in $path: $!";
	print $fh $bytes or die "could not write $path: $!";
	close $fh;
	return;
}

# The status lines showing the posture of a data directory
sub posture_like
{
	my ($datadir, $expected, $test_name) = @_;
	command_like([ pg_inplace_upgrade($datadir), 'status' ],
		$expected, $test_name);
	return;
}

# The new version's own cluster, never upgraded: its control file is in the
# current layout, out of upgrade mode, and PG_VERSION names this version.
my $fresh = PostgreSQL::Test::Cluster->new('fresh');
$fresh->init(force_initdb => 1);
my $new_major = slurp_file($fresh->data_dir . '/PG_VERSION');
chomp $new_major;

posture_like(
	$fresh->data_dir,
	qr/^Role:\s+primary\n.*^Owner:\s+new version\n.*^Upgrade posture:\s+live$/ms,
	'a cluster of the new version is live');

# Two major versions can share a control file layout: out of upgrade mode,
# PG_VERSION tells whose the cluster is.
my $datadir = forge($fresh->data_dir, 'fresh_old_pg_version');
append_to_file("$datadir/PG_VERSION.new", ($new_major - 1) . "\n");
rename "$datadir/PG_VERSION.new", "$datadir/PG_VERSION"
  or die "could not rename PG_VERSION: $!";
posture_like(
	$datadir,
	qr/^Owner:\s+old version\n.*^Upgrade posture:\s+not prepared$/ms,
	'a control file in the current layout with an older PG_VERSION is the old version\'s'
);

# Facts that cannot be read are reported, not guessed at.
$datadir = forge($fresh->data_dir, 'fresh_bad_pg_version');
unlink "$datadir/PG_VERSION";
append_to_file("$datadir/PG_VERSION", "garbage\n");
command_fails_like(
	[ pg_inplace_upgrade($datadir), 'status' ],
	qr/PG_VERSION" does not contain a valid major version/,
	'status refuses an invalid PG_VERSION');

$datadir = forge($fresh->data_dir, 'fresh_unknown_layout');
write_file("$datadir/global/pg_control", 8, pack('L', 1234));
command_fails_like(
	[ pg_inplace_upgrade($datadir), 'status' ],
	qr/control file version 1234 is not one this version can upgrade from/,
	'status refuses a control file of an unknown layout');

$datadir = forge($fresh->data_dir, 'fresh_bad_checksum');
write_file("$datadir/global/pg_control", 0, pack('L', 0xdeadbeef));
command_fails_like(
	[ pg_inplace_upgrade($datadir), 'status' ],
	qr/incorrect checksum in control file/,
	'status refuses a control file with a bad checksum');

# A cluster of the old version, prepared for the upgrade
my $node =
  PostgreSQL::Test::Cluster->new('node', install_path => $oldinstall);
$node->init(force_initdb => 1);
posture_like(
	$node->data_dir,
	qr/^Owner:\s+old version\n.*^Upgrade posture:\s+not prepared$/ms,
	'a cluster of the old version is not prepared');
$node->start;
$node->command_ok([ pg_inplace_upgrade($node->data_dir), 'prepare' ],
	'prepared');
$node->_update_pid(0);
posture_like(
	$node->data_dir,
	qr/^Owner:\s+old version\n.*^Upgrade posture:\s+at an upgrade boundary$/ms,
	'prepared, the old version is at an upgrade boundary');

done_testing();
