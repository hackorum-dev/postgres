
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test parallel autovacuum behavior

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Before each test we should disable autovacuum for 'test_autovac' table and
# generate some dead tuples in it.
sub prepare_for_next_test
{
	my ($node, $test_number) = @_;

	$node->safe_psql(
		'postgres', qq{
		ALTER TABLE test_autovac SET (autovacuum_enabled = false);
		UPDATE test_autovac SET col_1 = $test_number;
	});
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

# Limit to one autovacuum worker and disable autovacuum logging globally
# (enabled only on the test table) so that log checks below match only
# activity on the expected table.
#
# Effectively disable autovacuum for all tables except the ones the test
# re-enables via reloptions.  A worker spawned by catalog churn would skew
# the cost balance, and an injection point attached below would trap it,
# eating the only free worker slot.
$node->append_conf(
	'postgresql.conf', qq{
autovacuum_max_workers = 1
autovacuum_worker_slots = 2
autovacuum_max_parallel_workers = 2
max_worker_processes = 10
max_parallel_workers = 10
log_min_messages = debug2
autovacuum_naptime = '1s'
min_parallel_index_scan_size = 0
log_autovacuum_min_duration = -1
autovacuum_vacuum_threshold = 100000
autovacuum_analyze_threshold = 100000
autovacuum_vacuum_insert_threshold = -1
});
$node->start;

# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

# Create all functions needed for testing
$node->safe_psql(
	'postgres', qq{
	CREATE EXTENSION injection_points;
});

my $indexes_num = 3;
my $initial_rows_num = 10_000;
my $autovacuum_parallel_workers = 2;

# Create table and fill it with some data
$node->safe_psql(
	'postgres', qq{
	CREATE TABLE test_autovac (
		id SERIAL PRIMARY KEY,
		col_1 INTEGER,  col_2 INTEGER,  col_3 INTEGER,  col_4 INTEGER
	) WITH (autovacuum_parallel_workers = $autovacuum_parallel_workers,
			autovacuum_vacuum_threshold = 50,
			log_autovacuum_min_duration = 0);

	INSERT INTO test_autovac
	SELECT
		g AS col1,
		g + 1 AS col2,
		g + 2 AS col3,
		g + 3 AS col4
	FROM generate_series(1, $initial_rows_num) AS g;
});

# Create specified number of b-tree indexes on the table
$node->safe_psql(
	'postgres', qq{
	DO \$\$
	DECLARE
		i INTEGER;
	BEGIN
		FOR i IN 1..$indexes_num LOOP
			EXECUTE format('CREATE INDEX idx_col_\%s ON test_autovac (col_\%s);', i, i);
		END LOOP;
	END \$\$;
});

# Test 1 :
# Our table has enough indexes and appropriate reloptions, so autovacuum must
# be able to process it in parallel mode. Just check if it can do it.

prepare_for_next_test($node, 1);
my $log_offset = -s $node->logfile;

$node->safe_psql(
	'postgres', qq{
	ALTER TABLE test_autovac SET (autovacuum_enabled = true);
});

# Wait for parallel autovacuum to complete; check worker count matches reloptions.
$node->wait_for_log(
	qr/parallel workers: index vacuum: 2 planned, 2 launched in total/,
	$log_offset);
ok(1, "parallel autovacuum on test_autovac table");

# Test 2:
# Check whether parallel autovacuum leader can propagate cost-based parameters
# to the parallel workers.

prepare_for_next_test($node, 2);
$log_offset = -s $node->logfile;

$node->safe_psql(
	'postgres', qq{
	SELECT injection_points_attach('autovacuum-start-parallel-vacuum', 'wait');

	ALTER TABLE test_autovac SET (autovacuum_parallel_workers = 1, autovacuum_enabled = true);
});

# Wait until parallel autovacuum is inited
$node->wait_for_event('autovacuum worker',
	'autovacuum-start-parallel-vacuum');

# Update the shared cost-based delay parameters.
$node->safe_psql(
	'postgres', qq{
	ALTER SYSTEM SET autovacuum_vacuum_cost_limit = 500;
	ALTER SYSTEM SET autovacuum_vacuum_cost_delay = 5;
	ALTER SYSTEM SET vacuum_cost_page_miss = 10;
	ALTER SYSTEM SET vacuum_cost_page_dirty = 10;
	ALTER SYSTEM SET vacuum_cost_page_hit = 10;
	ALTER SYSTEM SET track_cost_delay_timing = on;
	SELECT pg_reload_conf();
});

# Resume the leader process to update the shared parameters during heap scan (i.e.
# vacuum_delay_point() is called) and launch a parallel vacuum worker, but it stops
# before vacuuming indexes due to the injection point.
$node->safe_psql(
	'postgres', qq{
	SELECT injection_points_wakeup('autovacuum-start-parallel-vacuum');
});

# Check whether parallel worker successfully updated all parameters during
# index processing.
$node->wait_for_log(
	qr/parallel autovacuum worker updated cost params: cost_limit=500, cost_delay=5, cost_page_miss=10, cost_page_dirty=10, cost_page_hit=10, track_cost_delay_timing=yes/,
	$log_offset);

# Cleanup
$node->safe_psql(
	'postgres', qq{
	SELECT injection_points_detach('autovacuum-start-parallel-vacuum');
});

ok(1,
	"vacuum delay parameter changes are propagated to parallel vacuum workers"
);

# Test 3:
# Check whether a cost limit rebalance reaches the parallel workers. The
# leader pauses right after taking the shared cost param snapshot
# (balance = 1, limit 500), then a second autovacuum worker joins the
# balance (balance = 2, limit 250) and is held there for the rest of the
# test. After resume, the parallel workers' first parameter load must show
# the rebalanced 250, not the snapshotted 500.

# Second worker's table lives in another database: no cost reloptions, so it
# participates in balancing.
$node->safe_psql('postgres', 'CREATE DATABASE regress_db2');
$node->safe_psql(
	'regress_db2', qq{
	CREATE TABLE filler (id int)
		WITH (autovacuum_enabled = false, autovacuum_vacuum_threshold = 50);
	INSERT INTO filler SELECT g FROM generate_series(1, 1000) g;
});

# Allow a second autovacuum worker.
$node->safe_psql(
	'postgres', qq{
	ALTER SYSTEM SET autovacuum_max_workers = 2;
	SELECT pg_reload_conf();
});

prepare_for_next_test($node, 3);
$node->safe_psql('regress_db2', 'UPDATE filler SET id = id + 1');

my $db2oid = $node->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'regress_db2'");
my $filleroid =
  $node->safe_psql('regress_db2', "SELECT 'filler'::regclass::oid");

$log_offset = -s $node->logfile;

# Pause the leader after the shared cost param snapshot. The leader is past
# the hold point below by then, so that one only catches the second worker.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('autovacuum-start-parallel-vacuum', 'wait')"
);
$node->safe_psql('postgres',
	'ALTER TABLE test_autovac SET (autovacuum_enabled = true)');
$node->wait_for_event('autovacuum worker',
	'autovacuum-start-parallel-vacuum');

# Second worker -> balance = 2. Hold it there: if it were allowed to finish,
# the balance would drop back to 1 before the leader resumes.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('autovacuum-worker-cost-balanced', 'wait')"
);
$node->safe_psql('regress_db2',
	'ALTER TABLE filler SET (autovacuum_enabled = true)');
$node->wait_for_log(
	qr/VacuumUpdateCosts\(db=$db2oid, rel=$filleroid, dobalance=yes, cost_limit=250,/,
	$log_offset);

$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('autovacuum-start-parallel-vacuum')");
$node->safe_psql('postgres',
	"SELECT injection_points_detach('autovacuum-start-parallel-vacuum')");

# First param load must show the rebalanced limit.
$node->wait_for_log(
	qr/parallel autovacuum worker updated cost params: cost_limit=\d+,/,
	$log_offset);
my $log = slurp_file($node->logfile, $log_offset);
my @limits =
  $log =~ /parallel autovacuum worker updated cost params: cost_limit=(\d+),/g;
note("parallel worker cost_limit sequence: @limits");
is($limits[0], '250', 'parallel workers see the rebalanced cost limit');

# Release the second worker.
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('autovacuum-worker-cost-balanced')");
$node->safe_psql('postgres',
	"SELECT injection_points_detach('autovacuum-worker-cost-balanced')");
$node->wait_for_log(
	qr/automatic vacuum of table "postgres\.public\.test_autovac"/,
	$log_offset);
ok( $node->poll_query_until(
		'postgres', q{
		SELECT count(*) = 0 FROM pg_stat_activity
		WHERE backend_type = 'autovacuum worker' AND datname = 'regress_db2'
	}),
	'second autovacuum worker finished');

# Test 4:
# Check whether a config reload is serviced while the autovacuum leader waits
# for its parallel worker.

$node->safe_psql(
	'postgres', qq{
	ALTER SYSTEM SET autovacuum_max_workers = 1;
	ALTER SYSTEM SET autovacuum_vacuum_cost_limit = 700;
	ALTER SYSTEM SET autovacuum_vacuum_cost_delay = 0;
	SELECT pg_reload_conf();
});

prepare_for_next_test($node, 4);
$log_offset = -s $node->logfile;

# Let an autovacuum worker process test_autovac with its parallel worker held
# before the parallel worker reads the cost-based delay parameters.  The
# leader then processes all indexes by itself and waits for the parallel
# worker to finish.  Change the parameters only once the leader is waiting,
# or it would pick up the change before reaching the code path under test.
$node->safe_psql(
	'postgres', q{
	SELECT injection_points_attach('parallel-vacuum-worker-start', 'wait');
	ALTER TABLE test_autovac SET (autovacuum_enabled = true);
});
$node->wait_for_event('autovacuum worker', 'ParallelFinish');

# Update cost-based delay parameters.
$node->safe_psql(
	'postgres', qq{
	ALTER SYSTEM SET autovacuum_vacuum_cost_limit = 800;
	ALTER SYSTEM SET autovacuum_vacuum_cost_delay = 8;
	ALTER SYSTEM SET vacuum_cost_page_miss = 11;
	ALTER SYSTEM SET vacuum_cost_page_dirty = 12;
	ALTER SYSTEM SET vacuum_cost_page_hit = 13;
	SELECT pg_reload_conf();
});

# The parallel worker reads the parameters only once, when it starts, since
# the leader has already processed all indexes.  So the leader must have
# propagated the new parameters before the parallel worker is released.
$node->wait_for_log(
	qr/parallel autovacuum leader propagated cost params: cost_limit=800,/,
	$log_offset);

# Release the parallel worker.  It reads the cost-based delay parameters the
# leader has propagated as soon as it resumes.
$node->safe_psql(
	'postgres',
	q{
	SELECT injection_points_detach('parallel-vacuum-worker-start');
	SELECT injection_points_wakeup('parallel-vacuum-worker-start');
});
$node->wait_for_log(
	qr/parallel autovacuum worker updated cost params: cost_limit=800, cost_delay=8, cost_page_miss=11, cost_page_dirty=12, cost_page_hit=13/,
	$log_offset);

# Wait for the autovacuum on test_autovac to finish.
$node->wait_for_log(
	qr/automatic vacuum of table "postgres\.public\.test_autovac"/,
	$log_offset);
ok(1, "config reload is propagated while the leader waits for workers");

# Test 5:
# Check the same wait path for a cost limit rebalance, which is not signaled
# by a config reload.  A second autovacuum worker joins the balance while the
# leader waits for its parallel worker.
$node->safe_psql(
	'postgres', qq{
	ALTER SYSTEM SET autovacuum_max_workers = 2;
	ALTER SYSTEM SET autovacuum_vacuum_cost_limit = 600;
	SELECT pg_reload_conf();
});

prepare_for_next_test($node, 5);
$node->safe_psql('regress_db2',
	'ALTER TABLE filler SET (autovacuum_enabled = false)');
$node->safe_psql('regress_db2', 'UPDATE filler SET id = id + 1');

$log_offset = -s $node->logfile;

# As in Test 4, hold the parallel worker and wait for the leader to process
# all indexes and wait for the parallel worker to finish.
$node->safe_psql(
	'postgres', q{
	SELECT injection_points_attach('parallel-vacuum-worker-start', 'wait');
	ALTER TABLE test_autovac SET (autovacuum_enabled = true);
});
$node->wait_for_event('autovacuum worker', 'ParallelFinish');

# Hold the second worker, so that the number of autovacuum workers sharing
# the cost limit stays at 2 until the parallel worker has read the
# parameters.  Once the second worker finishes, the leader would propagate
# the original cost limit again.
$node->safe_psql(
	'postgres', q{
	SELECT injection_points_attach('autovacuum-worker-cost-balanced', 'wait');
});
$node->safe_psql('regress_db2',
	'ALTER TABLE filler SET (autovacuum_enabled = true)');

# Wait for the second worker to update its cost parameters.  It has
# recalculated the number of workers sharing the cost limit, now 2, and woken
# up the leader.
$node->wait_for_log(
	qr/VacuumUpdateCosts\(db=$db2oid, rel=$filleroid, dobalance=yes, cost_limit=300,/,
	$log_offset);

# Likewise, the leader must propagate the rebalanced cost limit before the
# parallel worker is released.
$node->wait_for_log(
	qr/parallel autovacuum leader propagated cost params: cost_limit=300,/,
	$log_offset);

# Release the parallel worker.  It reads the cost-based delay parameters the
# leader has propagated as soon as it resumes.
$node->safe_psql(
	'postgres',
	q{
	SELECT injection_points_detach('parallel-vacuum-worker-start');
	SELECT injection_points_wakeup('parallel-vacuum-worker-start');
});
$node->wait_for_log(
	qr/parallel autovacuum worker updated cost params: cost_limit=300,/,
	$log_offset);

# Release the second worker.
$node->safe_psql(
	'postgres',
	q{
	SELECT injection_points_detach('autovacuum-worker-cost-balanced');
	SELECT injection_points_wakeup('autovacuum-worker-cost-balanced');
});

# Wait for the autovacuum on test_autovac to finish.
$node->wait_for_log(
	qr/automatic vacuum of table "postgres\.public\.test_autovac"/,
	$log_offset);
ok(1, "cost rebalance is propagated while the leader waits for workers");

$node->stop;
done_testing();
