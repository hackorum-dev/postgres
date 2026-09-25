# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
autovacuum_max_workers = 1
autovacuum_max_parallel_workers = 1
autovacuum_naptime = '1s'
autovacuum_vacuum_cost_delay = '20ms'
autovacuum_vacuum_cost_limit = 200
log_min_messages = debug2
min_parallel_index_scan_size = 0
});
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql(
	'postgres', q{
	CREATE EXTENSION injection_points;
	CREATE TABLE test_autovac (id int, a int)
		WITH (autovacuum_enabled = false,
			autovacuum_parallel_workers = 1,
			autovacuum_vacuum_threshold = 0,
			autovacuum_vacuum_scale_factor = 0);
	INSERT INTO test_autovac
		SELECT g, g FROM generate_series(1, 100) g;
	CREATE INDEX test_autovac_id_idx ON test_autovac (id);
	CREATE INDEX test_autovac_a_idx ON test_autovac (a);
	UPDATE test_autovac SET a = a + 1;
	SELECT injection_points_attach(
		'parallel-autovacuum-worker-before-index', 'wait');
	SELECT injection_points_attach(
		'parallel-autovacuum-leader-before-index', 'wait');
	SELECT injection_points_attach(
		'parallel-autovacuum-leader-after-worker-wait', 'notice');
	ALTER TABLE test_autovac SET (autovacuum_enabled = true);
});

$node->wait_for_event('autovacuum worker',
	'parallel-autovacuum-leader-before-index');
$node->wait_for_event('parallel worker',
	'parallel-autovacuum-worker-before-index');
$node->safe_psql(
	'postgres', q{
	SELECT injection_points_wakeup(
		'parallel-autovacuum-leader-before-index');
	SELECT injection_points_detach(
		'parallel-autovacuum-leader-before-index');
});
$node->poll_query_until(
	'postgres', q{
	SELECT EXISTS (
		SELECT 1
		FROM pg_stat_activity
		WHERE backend_type = 'autovacuum worker'
		AND wait_event = 'ParallelFinish')
}) or die "autovacuum leader did not reach ParallelFinish";

my $log_offset = -s $node->logfile;
if (!$ENV{NO_RELOAD_CONTROL})
{
	$node->safe_psql(
		'postgres', q{
		ALTER SYSTEM SET autovacuum_vacuum_cost_delay = 0;
		SELECT pg_reload_conf();
	});
}
$node->safe_psql(
	'postgres', q{
	SELECT injection_points_wakeup(
		'parallel-autovacuum-worker-before-index');
	SELECT injection_points_detach(
		'parallel-autovacuum-worker-before-index');
});

$node->wait_for_log(
	qr/parallel-autovacuum-leader-after-worker-wait \(reload (?:pending|processed)\)/,
	$log_offset);

my $log = slurp_file($node->logfile, $log_offset);
my ($reload_state) =
	$log =~ /parallel-autovacuum-leader-after-worker-wait \(reload (pending|processed)\)/;
is($reload_state, 'processed',
	'autovacuum leader processes a configuration reload while waiting');

$node->safe_psql(
	'postgres', q{
	SELECT injection_points_detach(
		'parallel-autovacuum-leader-after-worker-wait');
});
$node->stop;

done_testing();
