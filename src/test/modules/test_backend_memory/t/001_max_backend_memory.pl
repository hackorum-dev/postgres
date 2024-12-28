# Tests to check backend memory limit (max_backend_memory GUC)

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# Initialize backend memory limit
$node->safe_psql('postgres', qq{
	ALTER SYSTEM SET max_backend_memory = 15;
	SELECT pg_reload_conf();
});

# Check backend memory limit after SET.
my $psql_stdout;
$psql_stdout = $node->safe_psql('postgres',
								"SELECT current_setting('max_backend_memory');");
is($psql_stdout, '15MB', "max_backend_memory is SET correctly");

# Create test table and function.
$node->safe_psql('postgres', q{
	CREATE TABLE test(t text);
	INSERT INTO test VALUES (repeat('1234567890', 400000));

	-- Recursive function that should cause overflow
	CREATE FUNCTION test_func() RETURNS void LANGUAGE plpgsql AS $$
	DECLARE
		bt text;
	BEGIN
		SELECT t || 'x' FROM test INTO bt;
		PERFORM test_func();
	END;
$$;
});

# Call function "test_func" several times for memory leak check.
my $psql_stdout_first;
for (my $i = 0; $i < 4; $i++)
{
	# Call function and check that it finishes with 'out of memory' error.
	my ($ret, $stdout, $stderr) = $node->psql('postgres',
											  "SELECT test_func();");
	is($ret, 3, 'recursive function call causes overflow');
	like($stderr, qr/out of memory/, 'expected out of memory error');

	$psql_stdout = $node->safe_psql('postgres',
									"SELECT pg_get_backend_memory_contexts_total_bytes();");
	if ($i eq 0)
	{
		# Store first value of backend_memory_contexts_total_bytes.
		$psql_stdout_first = $psql_stdout;
		is($psql_stdout_first > 0, 1,
		   "backend has allocated $psql_stdout_first (greater than 0) bytes");
		next;
	}
	# Check other values of backend_memory_contexts_total_bytes.
	# They should be the same as first value.
	is($psql_stdout, $psql_stdout_first, "memory does not leak");
}

# Drop test table and function.
$node->safe_psql('postgres', q{
	DROP FUNCTION test_func();
	DROP TABLE test;
});

# Deinitialize backend memory limit.
$node->safe_psql('postgres', q{
	ALTER SYSTEM RESET max_backend_memory;
	SELECT pg_reload_conf();
});

# Check backend memory limit after RESET.
$psql_stdout = $node->safe_psql('postgres',
								"SELECT current_setting('max_backend_memory');");
is($psql_stdout, '0', "max_backend_memory is RESET correctly");

$node->stop;

done_testing();
