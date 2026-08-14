# Copyright (c) 2026, PostgreSQL Global Development Group

# A crash between the checkpoint an online enable requests and the control
# file write that follows it must not lose the transition.
#
# The last steps of SetDataChecksumsOn() are
#
#	WAL record -> shmem -> barrier -> checkpoint -> persist
#
# The checkpoint is what makes "on" safe to persist, but it also moves the
# redo point above the XLOG2_CHECKSUMS record that carries the new state.
# Crash recovery started from that checkpoint therefore never replays the
# record, so without the state the checkpoint itself persists, a crash in the
# remaining window would bring the cluster back at "inprogress-on", which
# StartupXLOG() resolves to "off", discarding a transition whose pages are all
# on disk with a checksum.
#
# t/023 exercises the same window through a checkpoint requested by another
# session.  This test closes it from the other side: the transition's own
# checkpoint is the one that moves the redo point, so persisting the state
# from CreateCheckPoint() is what has to save it, not the write below the
# injection point.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('crash_after_checkpoint');
$node->init(no_data_checksums => 1);
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 10GB
));
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

test_checksum_state($node, 'off');

$node->safe_psql('postgres',
	'CREATE TABLE t AS SELECT generate_series(1,10000) AS a;');

# Hold the launcher after the checkpoint that licenses "on" has completed and
# before the state reaches the control file.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('datachecksums-on-after-checkpoint','wait');"
);

enable_data_checksums($node);
$node->wait_for_event('datachecksums launcher',
	'datachecksums-on-after-checkpoint');

# The transition is complete as far as the running cluster is concerned.
test_checksum_state($node, 'on');

# The checkpoint has already recorded it, so the pending write below the
# injection point has nothing left to do.
my ($ctl) = run_command([ 'pg_controldata', $node->data_dir ]);
my ($version) = $ctl =~ /Data page checksum version:\s+(\d+)/;
is($version, '1',
	'the requested checkpoint recorded "on" in the control file');

# Crash before the control file write that follows the checkpoint.
$node->stop('immediate');

$node->start;

# Every page on disk carries a checksum and the checkpoint that flushed them
# completed, so the cluster has to come back verifying them.
test_checksum_state($node, 'on');

is($node->safe_psql('postgres', 'SELECT count(*) FROM t;'),
	'10000', 'relation readable after the crash');

my $log = PostgreSQL::Test::Utils::slurp_file($node->logfile);
unlike(
	$log,
	qr/enabling data checksums was interrupted/,
	'the completed transition is not reported as interrupted');

# The data directory must be one the offline tools accept.
$node->stop;
$node->command_ok([ 'pg_checksums', '--check', '-D', $node->data_dir ],
	'pg_checksums accepts the data directory');

done_testing();
