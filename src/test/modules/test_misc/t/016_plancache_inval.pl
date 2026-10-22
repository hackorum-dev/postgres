# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that a cached plan does not stay stale when an invalidation for an
# object that is only a planner-added dependency (a partition below the
# top-level parent, or a function that appears only after inlining) is
# processed while the plan is being built.
#
# Session A is paused with an injection point at a spot where planning has
# already read the catalog state in question, and will next acquire a new
# lock (and so process pending invalidation messages).  Another session
# changes that state.  After A finishes, every later execution of the
# cached plan must give the same answer as the same query run unprepared.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

# Sub-partitioned table: mid is a partition of root, and is itself
# partitioned.  float_sub is a standalone table that has rows only in mid's
# key range, so attaching or detaching it changes the answer.
my $partitioned_setup = q{
CREATE TABLE root (grp int, k int, v int) PARTITION BY LIST (grp);
CREATE TABLE p_a PARTITION OF root FOR VALUES IN (1);
CREATE TABLE mid PARTITION OF root FOR VALUES IN (2) PARTITION BY RANGE (k);
CREATE TABLE mid_1 PARTITION OF mid FOR VALUES FROM (0) TO (1000);
CREATE TABLE float_sub (grp int, k int, v int);
INSERT INTO p_a SELECT 1, i, i FROM generate_series(1, 100) i;
INSERT INTO mid_1 SELECT 2, i, i FROM generate_series(1, 100) i;
INSERT INTO float_sub SELECT 2, 1000 + i, i FROM generate_series(1, 100) i;
};

my $attach_float_sub = q{
ALTER TABLE mid ATTACH PARTITION float_sub FOR VALUES FROM (1000) TO (2000);
};

my $count_query = 'SELECT count(*) FROM root WHERE v >= 0';

my %cases = (
	# ATTACH PARTITION to a sub-partitioned table, generic plan
	attach => {
		setup => $partitioned_setup,
		prepare => [
			'SET plan_cache_mode = force_generic_plan;',
			'PREPARE q(int) AS SELECT count(*) FROM root WHERE v >= $1;',
		],
		point => 'expand-subpartdesc-read',
		execute => 'EXECUTE q(0);',
		unprepared => $count_query,
		ddl => $attach_float_sub,
	},

	# Same, but plan_cache_mode = auto: five custom plans first, so that
	# the paused execution is the first one to build a generic plan.
	attach_auto => {
		setup => $partitioned_setup,
		prepare => [
			'SET plan_cache_mode = auto;',
			'PREPARE q(int) AS SELECT count(*) FROM root WHERE v >= $1;',
		],
		warmup => 5,
		counters => 1,
		point => 'expand-subpartdesc-read',
		execute => 'EXECUTE q(0);',
		unprepared => $count_query,
		ddl => $attach_float_sub,
	},

	# DETACH PARTITION CONCURRENTLY on the sub-partitioned table, with the
	# second transaction cancelled while it waits for session A, which
	# leaves float_sub detach-pending.
	detach_cancelled => {
		setup => $partitioned_setup . $attach_float_sub,
		prepare => [
			'SET plan_cache_mode = force_generic_plan;',
			'PREPARE q(int) AS SELECT count(*) FROM root WHERE v >= $1;',
		],
		point => 'expand-subpartdesc-read',
		execute => 'EXECUTE q(0);',
		unprepared => $count_query,
		ddl => q{SET lock_timeout = '1s';
ALTER TABLE mid DETACH PARTITION float_sub CONCURRENTLY;},
		ddl_error => qr/canceling statement due to lock timeout/,
	},

	# Nested inlined SQL functions: the query references only outer_f, and
	# inner_f shows up only when outer_f is inlined.
	nested_inline => {
		setup => q{
CREATE FUNCTION inner_f(int) RETURNS int LANGUAGE sql IMMUTABLE
	AS 'SELECT $1 + 1';
CREATE FUNCTION outer_f(int) RETURNS int LANGUAGE sql IMMUTABLE
	AS 'SELECT inner_f($1)';
CREATE TABLE t (v int);
INSERT INTO t SELECT generate_series(1, 100);
CREATE INDEX ON t (v);
},
		prepare => [
			'SET plan_cache_mode = force_generic_plan;',
			'PREPARE q(int) AS SELECT sum(outer_f(v)) FROM t WHERE v >= $1;',
		],
		point => 'get-relation-info-start',
		execute => 'EXECUTE q(0);',
		unprepared => 'SELECT sum(outer_f(v)) FROM t WHERE v >= 0',
		ddl => q{CREATE OR REPLACE FUNCTION inner_f(int) RETURNS int
	LANGUAGE sql IMMUTABLE AS 'SELECT $1 + 1000';},
	},

	# The ATTACH case through a PL/pgSQL function, i.e. an SPI saved plan
	plpgsql => {
		setup => $partitioned_setup . q{
CREATE FUNCTION f_count(p int) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
  SELECT count(*) INTO n FROM root WHERE v >= p;
  RETURN n;
END $$;
},
		prepare => ['SET plan_cache_mode = force_generic_plan;'],
		point => 'expand-subpartdesc-read',
		execute => 'SELECT f_count(0);',
		unprepared => $count_query,
		ddl => $attach_float_sub,
	},);

# Create a fresh database for one case and run its setup.
sub setup_case
{
	my ($db, $case) = @_;

	$node->safe_psql('postgres', "CREATE DATABASE $db");
	$node->safe_psql($db, 'CREATE EXTENSION injection_points;');
	$node->safe_psql($db, $case->{setup});
}

# Run the DDL in another session.  It must not block on session A, except
# for the second transaction of DETACH CONCURRENTLY, which is expected to
# wait for session A and be cancelled.  Without a paused session A (the
# control runs), that DDL just completes.
sub run_ddl
{
	my ($db, $name, $case, $paused) = @_;

	my ($ret, $stdout, $stderr) = $node->psql($db, $case->{ddl});
	if ($paused && defined $case->{ddl_error})
	{
		like($stderr, $case->{ddl_error}, "$name: DDL fails as expected");
	}
	else
	{
		is($ret, 0, "$name: DDL succeeds") or diag $stderr;
	}
}

# Differential check: within one REPEATABLE READ transaction of session A,
# the cached plan must give the same result as the unprepared query.
sub check_cached_plan
{
	my ($session, $name, $case) = @_;

	for my $round (1 .. 3)
	{
		$session->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ;');
		my $cached = $session->query_safe($case->{execute});
		my $expected = $session->query_safe($case->{unprepared});
		$session->query_safe('COMMIT;');
		is($cached, $expected,
			"$name: cached plan matches unprepared query, round $round");
	}
}

# Return "generic|custom" plan counts of prepared statement q.
sub plan_counts
{
	my ($session) = @_;

	return $session->query_safe(
		q{
SELECT generic_plans || '|' || custom_plans
FROM pg_prepared_statements WHERE name = 'q';
});
}

# Run one case with session A paused at the injection point while the DDL
# runs.
sub run_paused
{
	my ($name, $case) = @_;
	my $db = "paused_$name";
	my $point = $case->{point};

	setup_case($db, $case);

	my $session = $node->background_psql($db);
	$session->query_safe($_) for @{ $case->{prepare} };
	$session->query_safe($case->{execute}) for 1 .. ($case->{warmup} // 0);
	if ($case->{counters})
	{
		is(plan_counts($session), '0|5',
			"$name: five custom plans before pause");
	}

	# Only session A should stop at the point.
	$session->query_safe('SELECT injection_points_set_local();');
	$session->query_safe("SELECT injection_points_attach('$point', 'wait');");
	$session->query_until(
		qr/starting_bg_psql/, qq(
\\echo starting_bg_psql
$case->{execute}
));
	$node->wait_for_event('client backend', $point);

	run_ddl($db, $name, $case, 1);

	# Detach first, so that the point cannot be hit again, then wake up A.
	$node->safe_psql(
		$db, qq{
SELECT injection_points_detach('$point');
SELECT injection_points_wakeup('$point');
});

	# Wait for the paused statement to complete.  Its result can legitimately
	# reflect the old catalog state, so it is not checked.
	$session->query_safe('SELECT 1;');

	if ($case->{counters})
	{
		is(plan_counts($session), '1|5',
			"$name: paused execution built and used a generic plan");
	}

	check_cached_plan($session, $name, $case);

	$session->quit;
}

# Control: the same DDL, run between two executions without any pause.
sub run_control
{
	my ($name, $case) = @_;
	my $db = "control_$name";

	setup_case($db, $case);

	my $session = $node->background_psql($db);
	$session->query_safe($_) for @{ $case->{prepare} };
	$session->query_safe($case->{execute})
	  for 1 .. (($case->{warmup} // 0) + 1);

	run_ddl($db, "control $name", $case, 0);

	check_cached_plan($session, "control $name", $case);

	$session->quit;
}

for my $name (sort keys %cases)
{
	run_paused($name, $cases{$name});
	run_control($name, $cases{$name});
}

done_testing();
