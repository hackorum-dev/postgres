# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('jit_s390x');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
restart_after_crash = on
jit = on
});
$node->start;

my $version = $node->safe_psql('postgres', 'SELECT version()');
plan skip_all => 's390x-specific LLVM JIT test'
  unless $version =~ /s390x/;

my $jit_available =
  $node->safe_psql('postgres', 'SELECT pg_catalog.pg_jit_available()');
plan skip_all => 'LLVM JIT is not available'
  unless $jit_available eq 't';

$node->safe_psql(
	'postgres',
	q{
CREATE FUNCTION type_text(oid) RETURNS text
LANGUAGE sql STABLE
AS $$
  SELECT typname::text
  FROM pg_catalog.pg_type
  WHERE oid = $1
$$;
});

my ($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	q{
SET jit = on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = -1;
SET jit_optimize_above_cost = -1;
SET jit_expressions = on;
SET jit_tuple_deforming = off;

SELECT count(*)
FROM (
	SELECT oid
	FROM pg_catalog.pg_type
	ORDER BY oid
	LIMIT 7
) AS t
WHERE type_text(t.oid) = 'int2vector';
});

is($ret, 0, 'JIT-compiled SQL function expression does not crash the backend');
is($stdout, "1\n", 'JIT-compiled SQL function expression returns expected row');
diag($stderr) if $ret != 0 && $stderr ne '';

done_testing();
