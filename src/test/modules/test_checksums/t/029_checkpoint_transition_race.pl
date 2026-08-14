# Copyright (c) 2026, PostgreSQL Global Development Group

# A checkpoint racing SetDataChecksumsOn() between the insertion of the
# XLOG2_CHECKSUMS("on") record and the shared memory update must not insert
# an XLOG_CHECKPOINT_REDO record that follows the transition in WAL order
# while still carrying "inprogress-on".  Recovery resuming from such a redo
# point never replays the preceding transition record, comes up in
# "inprogress-on" and resolves the finished transition as interrupted.
#
# XLogChecksums() closes the window by inserting the record and publishing
# the new state under DataChecksumTransitionLock, which CreateCheckPoint()
# takes around sampling the state and inserting the redo record.  Here the
# launcher is held between the two steps at the
# datachecksums-on-before-publish injection point, and a concurrent
# CHECKPOINT has to block on the lock instead of completing inside the
# window.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

use IPC::Run;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('checkpoint_transition');
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

# The datachecksums-on-before-publish point fires inside a critical section,
# where the wait machinery must not allocate.  Waiting once at the
# datachecksums-enable-checksums-delay point, which the launcher runs outside
# the critical section, initializes it; see 050_redo_segment_missing.pl for
# the same recipe around create-checkpoint-run.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('datachecksums-enable-checksums-delay','wait');"
);

# Hold the launcher after the XLOG2_CHECKSUMS("on") record is in WAL but
# before the new state is published in shared memory.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('datachecksums-on-before-publish','wait');"
);

enable_data_checksums($node);
$node->wait_for_event('datachecksums launcher',
	'datachecksums-enable-checksums-delay');
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('datachecksums-enable-checksums-delay');");
$node->safe_psql('postgres',
	"SELECT injection_points_detach('datachecksums-enable-checksums-delay');");

$node->wait_for_event('datachecksums launcher',
	'datachecksums-on-before-publish');

# The record is in WAL, the published state is still the old one.
test_checksum_state($node, 'inprogress-on');

# A concurrent checkpoint.  It must block on DataChecksumTransitionLock
# before inserting its redo record rather than complete inside the window.
my $checkpointer = IPC::Run::start(
	[ 'psql', '-XAtq', '-d', $node->connstr('postgres'), '-c', 'CHECKPOINT;' ],
	'<' => '/dev/null',
	'>' => '/dev/null',
	'2>' => '/dev/null',
	IPC::Run::timer($PostgreSQL::Test::Utils::timeout_default));

ok( $node->poll_query_until(
		'postgres',
		"SELECT wait_event = 'DataChecksumTransition' "
		  . "FROM pg_stat_activity WHERE backend_type = 'checkpointer';"),
	'concurrent checkpoint blocks on DataChecksumTransitionLock');

# Release the launcher; the checkpoint then samples the published "on" and
# its redo record follows the transition record.
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('datachecksums-on-before-publish');");
$node->safe_psql('postgres',
	"SELECT injection_points_detach('datachecksums-on-before-publish');");

$checkpointer->finish;

# Crash while the launcher's own checkpoint may still be in flight.  Recovery
# resumes from the concurrent checkpoint's redo point, which now lies above
# the transition record and carries "on".
$node->stop('immediate');

my $log_offset = -s $node->logfile;
$node->start;

# The transition completed, so the cluster has to come back "on".
wait_for_checksum_state($node, 'on');

my $log = PostgreSQL::Test::Utils::slurp_file($node->logfile, $log_offset);
unlike(
	$log,
	qr/enabling data checksums was interrupted/,
	'the completed transition is not reported as interrupted');

$node->stop;

done_testing();
