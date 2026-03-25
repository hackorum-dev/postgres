# Copyright (c) 2025-2026, PostgreSQL Global Development Group
#
# Test that pg_prewarm yields its AccessShareLock when a conflicting
# DDL operation (TRUNCATE, DROP TABLE) is waiting, and that pg_prewarm
# reports an appropriate error afterwards.

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
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', q(
	CREATE EXTENSION pg_prewarm;
	CREATE EXTENSION injection_points;
));

# Create tables large enough to trigger the waiter check
# (> PREWARM_WAITER_CHECK_INTERVAL = 1024 blocks).
$node->safe_psql('postgres', q(
	CREATE TABLE trunc_test AS
		SELECT i, repeat('x', 200) AS padding
		FROM generate_series(1, 50000) i;
	CREATE TABLE drop_test AS
		SELECT i, repeat('x', 200) AS padding
		FROM generate_series(1, 50000) i;
));

my $nblocks = $node->safe_psql('postgres',
	"SELECT pg_relation_size('trunc_test') / current_setting('block_size')::int");
ok($nblocks > 1024, "trunc_test has more than 1024 blocks ($nblocks)");

# ------------------------------------------------------------------
# Test 1: Normal pg_prewarm on a large table with no conflicting waiters.
# ------------------------------------------------------------------
my $result = $node->safe_psql('postgres',
	"SELECT pg_prewarm('trunc_test', 'buffer')");
like($result, qr/^[1-9][0-9]*$/, 'pg_prewarm buffer mode succeeds normally');

# ------------------------------------------------------------------
# Test 2: TRUNCATE proceeds during pg_prewarm.
#
# pg_prewarm pauses at the injection point while holding AccessShareLock.
# TRUNCATE blocks waiting for AccessExclusiveLock.  After we wake up
# pg_prewarm it detects the waiter, yields, and TRUNCATE completes.
# pg_prewarm then errors because the relation was truncated.
# ------------------------------------------------------------------
$node->safe_psql('postgres',
	"SELECT injection_points_attach('pg_prewarm-before-check-and-yield', 'wait')");

my $prewarm = $node->background_psql('postgres', on_error_stop => 0);
$prewarm->query_until(qr/starting_prewarm/, q(
	\echo starting_prewarm
	SELECT pg_prewarm('trunc_test', 'buffer');
));

# Wait for pg_prewarm to hit the injection point.
$node->poll_query_until('postgres', q(
	SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'pg_prewarm-before-check-and-yield';
), 't');

# Start TRUNCATE in a second session — it will block on the lock.
my $truncate = $node->background_psql('postgres');
$truncate->query_until(qr/starting_truncate/, q(
	\echo starting_truncate
	TRUNCATE trunc_test;
));

# Confirm TRUNCATE is waiting for a lock.
$node->poll_query_until('postgres', q(
	SELECT count(*) > 0 FROM pg_stat_activity
	WHERE query LIKE '%TRUNCATE trunc_test%'
	AND wait_event_type = 'Lock';
), 't');

# Detach the injection point so pg_prewarm won't pause again at the
# next check interval, then wake it up.
$node->safe_psql('postgres',
	"SELECT injection_points_detach('pg_prewarm-before-check-and-yield')");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('pg_prewarm-before-check-and-yield')");

# TRUNCATE should now complete because pg_prewarm yielded its lock.
$truncate->quit;
pass('TRUNCATE completed during pg_prewarm');

# pg_prewarm should have reported that the relation was truncated.
$prewarm->quit;
like($prewarm->{stderr},
	qr/relation was truncated during pg_prewarm/,
	'pg_prewarm reports truncation after TRUNCATE');

# ------------------------------------------------------------------
# Test 3: DROP TABLE proceeds during pg_prewarm.
# ------------------------------------------------------------------
$node->safe_psql('postgres',
	"SELECT injection_points_attach('pg_prewarm-before-check-and-yield', 'wait')");

my $prewarm2 = $node->background_psql('postgres', on_error_stop => 0);
$prewarm2->query_until(qr/starting_prewarm/, q(
	\echo starting_prewarm
	SELECT pg_prewarm('drop_test', 'buffer');
));

$node->poll_query_until('postgres', q(
	SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'pg_prewarm-before-check-and-yield';
), 't');

my $drop = $node->background_psql('postgres');
$drop->query_until(qr/starting_drop/, q(
	\echo starting_drop
	DROP TABLE drop_test;
));

$node->poll_query_until('postgres', q(
	SELECT count(*) > 0 FROM pg_stat_activity
	WHERE query LIKE '%DROP TABLE drop_test%'
	AND wait_event_type = 'Lock';
), 't');

$node->safe_psql('postgres',
	"SELECT injection_points_detach('pg_prewarm-before-check-and-yield')");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('pg_prewarm-before-check-and-yield')");

$drop->quit;
pass('DROP TABLE completed during pg_prewarm');

$prewarm2->quit;
like($prewarm2->{stderr},
	qr/relation was dropped during pg_prewarm/,
	'pg_prewarm reports drop after DROP TABLE');

$node->stop;
done_testing();
