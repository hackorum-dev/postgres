# Copyright (c) 2026, PostgreSQL Global Development Group

# Check that a backend in single-user mode can do IO on temporary relations,
# and that this IO is reported in pg_stat_io.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Skip the tests on Windows, as single-user mode would fail on permission
# failure with privileged accounts.
if ($windows_os)
{
	plan skip_all => 'this test is not supported by this platform';
}

# Run set of queries in single-user mode.
sub test_single_mode
{
	my ($node, $queries, $testname) = @_;

	my $result = run_log(
		[
			'postgres', '--single', '-F',
			'-c' => 'exit_on_error=true',
			'-D' => $node->data_dir,
			'postgres'
		],
		'<' => \$queries);

	ok($result, $testname);
}

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;

test_single_mode(
	$node, qq(
CREATE TEMP TABLE temp_insert (a int);
INSERT INTO temp_insert VALUES (1);
),
	"INSERT into temporary table");
test_single_mode(
	$node,
	"CREATE TEMP TABLE temp_ctas AS SELECT 1 AS a;",
	"CREATE TEMP TABLE AS");
test_single_mode(
	$node, qq(
CREATE TEMP SEQUENCE temp_seq;
SELECT nextval('temp_seq');
),
	"nextval() on temporary sequence");

# A temporary table larger than temp_buffers, so as local buffers are
# evicted, written and read back.
test_single_mode(
	$node, qq(
SET temp_buffers = 100;
CREATE TEMP TABLE temp_big AS SELECT g AS a, repeat('x', 100) AS b FROM generate_series(1, 20000) g;
SELECT count(*) FROM temp_big;
),
	"temporary table larger than temp_buffers");

# The IO done on temporary relations by the standalone backend is visible
# once the server is started.
$node->start;
my $result = $node->safe_psql(
	'postgres', qq(
SELECT extends > 0, reads > 0, writes > 0, evictions > 0, hits > 0
  FROM pg_stat_io
  WHERE backend_type = 'standalone backend' AND
    object = 'temp relation' AND context = 'normal'));
is($result, 't|t|t|t|t',
	"IO on temporary relations of standalone backend in pg_stat_io");
$node->stop;

done_testing();
