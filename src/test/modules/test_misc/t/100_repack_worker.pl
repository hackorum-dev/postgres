# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that REPACK (CONCURRENTLY) does not wait forever for a decoding worker
# that goes away.  The worker can leave before it can report anything at all,
# leave after reporting an error, leave without a word, or be killed while it
# blocks writing into a full error message queue, either while decoding or on
# its way out.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Injection points not supported by this build'
  unless $ENV{enable_injection_points} eq 'yes';

my $node = PostgreSQL::Test::Cluster->new('node');
# REPACK (CONCURRENTLY) decodes WAL, so it needs more than wal_level=minimal.
$node->init(allows_streaming => 1);
$node->start;

plan skip_all => 'Extension injection_points not installed'
  unless $node->check_extension('injection_points');

$node->safe_psql(
	'postgres', qq[
	CREATE EXTENSION injection_points;
	CREATE TABLE tbl (i int PRIMARY KEY, j text);
	INSERT INTO tbl SELECT g, 'row ' || g FROM generate_series(1, 100) g;
]);

my $filenode =
  $node->safe_psql('postgres', "SELECT pg_relation_filenode('tbl')");

# Note that none of the injection points below is local to the session that
# attaches it, because they all have to fire in the decoding worker.
#
# Each REPACK below is given a timeout, so that a backend that waits forever
# for the worker, which is the bug being tested, fails the test instead of
# hanging it.

# A worker that leaves before it attaches to the error message queue leaves
# without signalling the backend, so the queue cannot report it.  The backend
# has to notice it while waiting for it to start.
{
	my $point = 'repack-worker-before-error-queue-attach';

	$node->safe_psql(
		'postgres', qq[
		SELECT injection_points_attach('$point', 'injection_points',
									   'injection_exit', NULL);
	]);

	my ($ret, $stdout, $stderr) = $node->psql(
		'postgres',
		'REPACK (CONCURRENTLY) tbl',
		timeout => $PostgreSQL::Test::Utils::timeout_default);
	isnt($ret, 0, "REPACK fails when the worker exits at $point");
	like(
		$stderr,
		qr/REPACK decoding worker failed to start/,
		"REPACK reports the worker that never attached to the queue");

	$node->safe_psql('postgres', "SELECT injection_points_detach('$point')");
}

# These are the three places where the worker has something to tell the
# backend, and thus the three places where the backend has something to wait
# for: the decoding setup, the initial snapshot, and the file of concurrent
# changes.
my @points = (
	'repack-worker-after-error-queue-attach',
	'repack-worker-before-snapshot-export',
	'repack-worker-before-changes-export');

# A worker that leaves without a word.  The backend only learns about it from
# the error message queue going away.
foreach my $point (@points)
{
	$node->safe_psql(
		'postgres', qq[
		SELECT injection_points_attach('$point', 'injection_points',
									   'injection_exit', NULL);
	]);

	my ($ret, $stdout, $stderr) = $node->psql(
		'postgres',
		'REPACK (CONCURRENTLY) tbl',
		timeout => $PostgreSQL::Test::Utils::timeout_default);
	isnt($ret, 0, "REPACK fails when the worker exits at $point");
	like(
		$stderr,
		qr/lost connection to REPACK decoding worker/,
		"REPACK reports the worker that exited at $point");

	$node->safe_psql('postgres', "SELECT injection_points_detach('$point')");
}

# A worker that reports an error before it leaves.  The error reaches the
# backend through the queue, so the backend reports that one instead.
foreach my $point (@points)
{
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('$point', 'error')");

	my ($ret, $stdout, $stderr) = $node->psql(
		'postgres',
		'REPACK (CONCURRENTLY) tbl',
		timeout => $PostgreSQL::Test::Utils::timeout_default);
	isnt($ret, 0, "REPACK fails when the worker errors out at $point");
	like(
		$stderr,
		qr/error triggered for injection point $point/,
		"REPACK reports the error of the worker at $point");

	$node->safe_psql('postgres', "SELECT injection_points_detach('$point')");
}

is($node->safe_psql('postgres', "SELECT pg_relation_filenode('tbl')"),
	$filenode, 'a failed REPACK leaves the table alone');

# A worker killed while it waits for the backend to read from a full error
# message queue.  The error it reports on the way out cannot reach the backend,
# because the queue it would go through is the one that is already full.
SKIP:
{
	skip 'this test requires SIGSTOP', 1 if $windows_os;

	# Hold the worker until we have stopped the backend, and then make it send
	# a message far larger than the error message queue.
	$node->safe_psql(
		'postgres', qq[
		SELECT injection_points_attach('repack-worker-before-snapshot-export',
									   'wait');
		SELECT injection_points_attach('repack-worker-after-snapshot-export',
									   'injection_points',
									   'injection_notice_oversized', NULL);
	]);

	my $psql = $node->background_psql('postgres', on_error_stop => 0);
	my $backend_pid = $psql->query('SELECT pg_backend_pid()');

	$psql->{stdin} .= "REPACK (CONCURRENTLY) tbl;\n";
	$psql->{run}->pump_nb();

	# Once the worker is held, the backend is waiting for it and has not yet
	# read anything from the queue.
	$node->poll_query_until(
		'postgres', qq[
		SELECT count(*) = 1 FROM pg_stat_activity
		WHERE backend_type = 'REPACK decoding worker'
			AND wait_event = 'repack-worker-before-snapshot-export'
	]) or die "timed out while waiting for the decoding worker to start";

	my $worker_pid = $node->safe_psql(
		'postgres', qq[
		SELECT pid FROM pg_stat_activity
		WHERE backend_type = 'REPACK decoding worker'
	]);

	# Stop the backend, so that nothing reads from the queue any more, and let
	# the worker fill it.
	kill 'STOP', $backend_pid;
	$node->safe_psql(
		'postgres', qq[
		SELECT injection_points_wakeup('repack-worker-before-snapshot-export');
	]);
	$node->poll_query_until(
		'postgres', qq[
		SELECT count(*) = 1 FROM pg_stat_activity
		WHERE pid = $worker_pid AND wait_event = 'MessageQueuePutMessage'
	]) or die "timed out while waiting for the error message queue to fill up";

	# Kill the worker while it waits, then let the backend run again.
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($worker_pid)");
	kill 'CONT', $backend_pid;

	ok( pump_until(
			$psql->{run}, $psql->{timeout},
			\$psql->{stderr},
			qr/lost connection to REPACK decoding worker/),
		'REPACK reports the worker killed while the queue was full');

	$psql->quit;

	$node->safe_psql(
		'postgres', qq[
		SELECT injection_points_detach('repack-worker-before-snapshot-export');
		SELECT injection_points_detach('repack-worker-after-snapshot-export');
	]);
}

# A worker blocked writing into a full error message queue on its way out, where
# proc_exit() holds interrupts and nothing can make it give up.  The backend has
# to stop watching the queue before it waits for the worker, or the two deadlock.
{
	# Hold the worker where the backend is waiting for it, and make it emit a
	# message far larger than the error message queue as it exits.
	$node->safe_psql(
		'postgres', qq[
		SELECT injection_points_attach('repack-worker-before-snapshot-export',
									   'wait');
		SELECT injection_points_attach('repack-worker-before-exit',
									   'injection_points',
									   'injection_notice_oversized', NULL);
	]);

	my $psql = $node->background_psql('postgres', on_error_stop => 0);
	my $backend_pid = $psql->query('SELECT pg_backend_pid()');

	$psql->{stdin} .= "REPACK (CONCURRENTLY) tbl;\n";
	$psql->{run}->pump_nb();

	# While the worker is held, only it can export the snapshot the backend
	# waits for, so the backend is asleep and reads nothing from the queue.
	$node->poll_query_until(
		'postgres', qq[
		SELECT count(*) = 1 FROM pg_stat_activity
		WHERE backend_type = 'REPACK decoding worker'
			AND wait_event = 'repack-worker-before-snapshot-export'
	]) or die "timed out while waiting for the decoding worker to start";

	# Cancel the backend, which sends it into teardown with the worker alive.
	$node->safe_psql('postgres', "SELECT pg_cancel_backend($backend_pid)");

	ok( pump_until(
			$psql->{run}, $psql->{timeout},
			\$psql->{stderr},
			qr/canceling statement due to user request/),
		'REPACK stops the worker that blocks on the queue while exiting');

	$psql->quit;

	$node->safe_psql(
		'postgres', qq[
		SELECT injection_points_detach('repack-worker-before-snapshot-export');
		SELECT injection_points_detach('repack-worker-before-exit');
	]);
}

# Nothing above left the table or the session in a state that keeps REPACK from
# working.
$node->safe_psql('postgres', 'REPACK (CONCURRENTLY) tbl');
isnt($node->safe_psql('postgres', "SELECT pg_relation_filenode('tbl')"),
	$filenode, 'REPACK rewrites the table once the worker is left alone');
is($node->safe_psql('postgres', 'SELECT count(*), sum(i) FROM tbl'),
	'100|5050', 'REPACK keeps the table data');

done_testing();
