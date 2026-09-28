# Copyright (c) 2026, PostgreSQL Global Development Group

# Verify that the backup size estimate reported for an incremental backup
# reflects the amount of data that will actually be sent, rather than the size
# of a full backup.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'summarize_wal = on');
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

# Most of the data that will not change goes into a tablespace of its own, so
# that the estimate is exercised both for $PGDATA and for a tablespace. An
# in-place tablespace needs no mapping when the backup is taken in tar format.
$node->safe_psql('postgres', <<EOM);
SET allow_in_place_tablespaces = on;
CREATE TABLESPACE inplace LOCATION '';
CREATE TABLE unchanged TABLESPACE inplace AS
  SELECT g AS i, repeat('x', 500) AS pad FROM generate_series(1, 20000) g;
CREATE TABLE changed AS SELECT 1 AS i;
CHECKPOINT;
EOM

# Runs a backup with --progress and returns the two amounts reported by the
# last progress line, in kB: what was streamed and what was estimated.
sub reported_progress
{
	my ($path, @extra) = @_;
	my ($stdout, $stderr) = ('', '');

	my $result = IPC::Run::run(
		[
			'pg_basebackup',
			'--host' => $node->host,
			'--port' => $node->port,
			'--pgdata' => $path,
			'--format' => 'tar',
			'--wal-method' => 'none',
			'--no-sync',
			'--progress',
			'--checkpoint' => 'fast',
			@extra
		],
		'>' => \$stdout,
		'2>' => \$stderr);
	ok($result, "backup into $path succeeded")
	  or die "pg_basebackup failed: $stderr";

	# Progress lines are separated by carriage returns and look like
	# "  6519/346720 kB (1%), 1/2 tablespaces".
	my @lines = grep { /kB/ } split(/[\r\n]+/, $stderr);
	ok(@lines > 0, "progress was reported for $path")
	  or die "no progress output for $path";

	my ($streamed, $total) = $lines[-1] =~ m{(\d+)/(\d+) kB};
	defined($total)
	  or die "could not parse progress line for $path: $lines[-1]";
	return ($streamed, $total);
}

my $backup_dir = $node->backup_dir;
my ($full_streamed, $full_total) = reported_progress("$backup_dir/full");

# A small change, so that the incremental backup has very little to send.
$node->safe_psql('postgres', <<EOM);
INSERT INTO changed SELECT g FROM generate_series(1, 1000) g;
CHECKPOINT;
EOM

my ($incr_streamed, $incr_total) = reported_progress("$backup_dir/incr",
	'--incremental' => "$backup_dir/full/backup_manifest");

note "full backup streamed $full_streamed of $full_total kB";
note "incremental backup streamed $incr_streamed of $incr_total kB";

# An incremental backup sends only a fraction of the cluster, so an estimate
# that still describes a full backup is far too large.
cmp_ok($incr_total, '<', $full_total / 2,
	'incremental backup estimate is much smaller than a full backup estimate'
);

# The estimate should also match what is really sent, in $PGDATA and in the
# tablespace alike. pg_basebackup raises the total to the amount streamed, so
# the two end up equal when the estimate is right, and the total stays far
# above it when any part of the cluster is still counted in full.
cmp_ok($incr_total, '<', $incr_streamed * 2,
	'incremental backup estimate is close to the amount streamed');

done_testing();
