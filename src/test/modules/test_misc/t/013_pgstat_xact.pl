# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test that pg_stat_xact_* views report only current-transaction activity,
# not accumulated pending stats from prior transactions.
#
# Use psql -c so all statements in a command are sent as one simple-query
# message.  That preserves pgStatPending entries across consecutive top-level
# transactions by avoiding a ReadyForQuery boundary, where PostgresMain() may
# call pgstat_report_stat().

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "track_functions = 'all'");
$node->append_conf('postgresql.conf', "max_prepared_transactions = 5");
$node->start;

my $dbname = 'postgres';

$node->safe_psql(
	$dbname, q{
	CREATE TABLE test_xact_stats (x int);
	CREATE FUNCTION test_xact_stats_func() RETURNS void
		LANGUAGE plpgsql AS $$ BEGIN NULL; END; $$;
});

sub psql_c_sequence_like
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $commands, $expected_stdout, $test_name) = @_;
	my ($stdout, $stderr);
	my @cmd = (
		$node->installed_command('psql'),
		'--no-psqlrc',
		'--no-align',
		'--tuples-only',
		'--quiet',
		'--set' => 'ON_ERROR_STOP=1',
		'-d' => $node->connstr($dbname));

	foreach my $command (@{$commands})
	{
		push @cmd, '-c' => $command;
	}

	my $result = IPC::Run::run \@cmd, '>', \$stdout, '2>', \$stderr;

	is($result, 1, "$test_name: exit code 0");
	is($stderr, '', "$test_name: no stderr");
	like($stdout, $expected_stdout, "$test_name: matches");
}

sub psql_c_like
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $sql, $expected_stdout, $test_name) = @_;

	psql_c_sequence_like($node, [$sql], $expected_stdout, $test_name);
}

psql_c_like(
	$node,
	q{BEGIN;
	INSERT INTO test_xact_stats VALUES (1);
	COMMIT;
	BEGIN;
	INSERT INTO test_xact_stats VALUES (2);
	SELECT 'n_tup_ins:' || n_tup_ins FROM pg_stat_xact_user_tables
		WHERE relname = 'test_xact_stats';
	COMMIT;},
	qr/^n_tup_ins:1$/m,
	"table n_tup_ins shows only current transaction's inserts");

psql_c_like(
	$node,
	q{BEGIN;
	SELECT count(*) FROM test_xact_stats;
	COMMIT;
	BEGIN;
	SELECT count(*) FROM test_xact_stats;
	SELECT 'seq_scan:' || seq_scan FROM pg_stat_xact_user_tables
		WHERE relname = 'test_xact_stats';
	COMMIT;},
	qr/^seq_scan:1$/m,
	"table seq_scan shows only current transaction's scans");

psql_c_like(
	$node,
	q{BEGIN;
	SELECT test_xact_stats_func();
	SELECT test_xact_stats_func();
	COMMIT;
	BEGIN;
	SELECT test_xact_stats_func();
	SELECT 'calls:' || calls FROM pg_stat_xact_user_functions
		WHERE funcname = 'test_xact_stats_func';
	COMMIT;},
	qr/^calls:1$/m,
	"function calls shows only current transaction's calls");

psql_c_like(
	$node,
	q{BEGIN;
	SELECT count(*) FROM test_xact_stats;
	PREPARE TRANSACTION 'test_prep';
	BEGIN;
	SELECT count(*) FROM test_xact_stats;
	SELECT 'seq_scan:' || seq_scan FROM pg_stat_xact_user_tables
		WHERE relname = 'test_xact_stats';
	COMMIT;},
	qr/^seq_scan:1$/m,
	"table seq_scan does not leak across PREPARE boundary");

$node->safe_psql($dbname, q{COMMIT PREPARED 'test_prep';});

psql_c_like(
	$node,
	q{BEGIN;
	SELECT count(*) FROM test_xact_stats;
	INSERT INTO test_xact_stats VALUES (99);
	COMMIT;
	BEGIN;
	SELECT 'seq_scan:' || seq_scan || ',n_tup_ins:' || n_tup_ins
		FROM pg_stat_xact_user_tables
		WHERE relname = 'test_xact_stats';
	COMMIT;},
	qr/^seq_scan:0,n_tup_ins:0$/m,
	"untouched relation entry shows zeroes in current transaction");

psql_c_like(
	$node,
	q{BEGIN;
	SELECT test_xact_stats_func();
	COMMIT;
	BEGIN;
	SELECT 'found:' || count(*) FROM pg_stat_xact_user_functions
		WHERE funcname = 'test_xact_stats_func';
	COMMIT;},
	qr/^found:0$/m,
	"uncalled function is not visible in pg_stat_xact_user_functions");

psql_c_like(
	$node,
	q{BEGIN;
	SELECT test_xact_stats_func();
	SELECT test_xact_stats_func();
	PREPARE TRANSACTION 'test_func_prep';
	BEGIN;
	SELECT test_xact_stats_func();
	SELECT 'calls:' || calls FROM pg_stat_xact_user_functions
		WHERE funcname = 'test_xact_stats_func';
	COMMIT;},
	qr/^calls:1$/m,
	"function calls do not leak across PREPARE boundary");

$node->safe_psql($dbname, q{COMMIT PREPARED 'test_func_prep';});

psql_c_sequence_like(
	$node,
	[
		q{BEGIN;
		INSERT INTO test_xact_stats VALUES (300);
		PREPARE TRANSACTION 'test_commit_prep';
		SELECT pg_stat_force_next_flush();},
		q{COMMIT PREPARED 'test_commit_prep';},
		q{BEGIN;
		SELECT 'n_tup_ins:' || n_tup_ins FROM pg_stat_xact_user_tables
			WHERE relname = 'test_xact_stats';
		COMMIT;}
	],
	qr/^n_tup_ins:0$/m,
	"table n_tup_ins does not leak after COMMIT PREPARED");

psql_c_sequence_like(
	$node,
	[
		q{BEGIN;
		INSERT INTO test_xact_stats VALUES (400);
		PREPARE TRANSACTION 'test_rollback_prep';
		SELECT pg_stat_force_next_flush();},
		q{ROLLBACK PREPARED 'test_rollback_prep';},
		q{BEGIN;
		SELECT 'n_tup_ins:' || n_tup_ins FROM pg_stat_xact_user_tables
			WHERE relname = 'test_xact_stats';
		COMMIT;}
	],
	qr/^n_tup_ins:0$/m,
	"table n_tup_ins does not leak after ROLLBACK PREPARED");

for my $gid (
	qw(test_prep test_func_prep test_commit_prep test_rollback_prep))
{
	eval { $node->safe_psql($dbname, "ROLLBACK PREPARED '$gid'"); };
}

$node->safe_psql(
	$dbname, q{
	DROP TABLE test_xact_stats;
	DROP FUNCTION test_xact_stats_func();
});

$node->stop;
done_testing();
