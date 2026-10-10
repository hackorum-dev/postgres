# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE DATABASE dump_src');
$node->safe_psql('postgres', 'CREATE DATABASE dump_dst');
$node->safe_psql(
	'dump_src',
	q{
-- A local UNIQUE constraint's index attached to a plain parent index.
CREATE TABLE a (id integer, k integer) PARTITION BY LIST (k);
CREATE TABLE a1 PARTITION OF a FOR VALUES IN (1);
ALTER TABLE a1 ADD CONSTRAINT a1_key UNIQUE (id, k);
ALTER TABLE a1 ADD CONSTRAINT a1_local UNIQUE (id);
CREATE UNIQUE INDEX a_idx ON ONLY a (id, k);
ALTER INDEX a_idx ATTACH PARTITION a1_key;
CREATE TABLE a2 PARTITION OF a FOR VALUES IN (2);
CREATE TABLE refs (id integer, k integer,
                   FOREIGN KEY (id, k) REFERENCES a1 (id, k));
INSERT INTO a VALUES (1, 1), (2, 2);
INSERT INTO refs VALUES (1, 1);

-- A local PRIMARY KEY two index partitions below the root.
CREATE TABLE b (id integer, k integer) PARTITION BY LIST (k);
CREATE TABLE b1 PARTITION OF b FOR VALUES IN (1) PARTITION BY RANGE (id);
CREATE TABLE b11 PARTITION OF b1 FOR VALUES FROM (0) TO (10);
ALTER TABLE b11 ADD CONSTRAINT b11_pkey PRIMARY KEY (id, k);
CREATE UNIQUE INDEX b1_idx ON ONLY b1 (id, k);
ALTER INDEX b1_idx ATTACH PARTITION b11_pkey;
CREATE UNIQUE INDEX b_idx ON ONLY b (id, k);
ALTER INDEX b_idx ATTACH PARTITION b1_idx;
INSERT INTO b VALUES (3, 1);

-- Plain index partitions do not prevent dropping their parent index.
CREATE TABLE c (id integer, k integer) PARTITION BY LIST (k);
CREATE TABLE c1 PARTITION OF c FOR VALUES IN (1);
CREATE UNIQUE INDEX c_idx ON c (id, k);

-- Coverage can come from a root index above a non-droppable intermediate one.
CREATE TABLE d (id integer, k integer) PARTITION BY LIST (k);
CREATE TABLE d1 PARTITION OF d FOR VALUES IN (1) PARTITION BY RANGE (id);
CREATE TABLE d11 PARTITION OF d1 FOR VALUES FROM (0) TO (10);
CREATE UNIQUE INDEX d_idx ON ONLY d (id, k);
CREATE UNIQUE INDEX d1_idx ON ONLY d1 (id, k);
CREATE UNIQUE INDEX d11_idx ON d11 (id, k);
ALTER INDEX d1_idx ATTACH PARTITION d11_idx;
ALTER INDEX d_idx ATTACH PARTITION d1_idx;
INSERT INTO d VALUES (4, 1);
});

$node->command_ok(
	[
		'pg_dump', '--clean', '--no-sync',
		'--file' => "$tempdir/clean.sql",
		'--dbname' => $node->connstr('dump_src'),
	],
	'plain clean dump of constraint-backed index partitions');
my $plain = slurp_file("$tempdir/clean.sql");
unlike(
	$plain,
	qr/DROP INDEX public\.a_idx;/,
	'omit parent index DROP with a constraint-backed partition');
unlike(
	$plain,
	qr/DROP INDEX public\.b_idx;/,
	'omit parent index DROP with a constraint-backed descendant');
like(
	$plain,
	qr/DROP INDEX public\.c_idx;/,
	'retain parent index DROP without constraint-backed partitions');
like(
	$plain,
	qr/DROP CONSTRAINT a1_local;/,
	'retain DROP of unattached local constraint');

$node->command_ok(
	[
		'pg_dump', '--format=custom', '--no-sync',
		'--file' => "$tempdir/partition.dump",
		'--dbname' => $node->connstr('dump_src'),
	],
	'custom dump of constraint-backed index partitions');
$node->command_ok(
	[
		'pg_restore', '--exit-on-error',
		'--dbname' => $node->connstr('dump_dst'),
		"$tempdir/partition.dump",
	],
	'initial restore of constraint-backed index partitions');

for my $option ('--exit-on-error', '--single-transaction',
	'--transaction-size=2')
{
	$node->command_ok(
		[
			'pg_restore', '--clean', $option,
			'--dbname' => $node->connstr('dump_dst'),
			"$tempdir/partition.dump",
		],
		"clean restore of constraint-backed index partitions with $option");
}
$node->command_ok(
	[
		'psql', '-X', '-v', 'ON_ERROR_STOP=1',
		'--dbname' => $node->connstr('dump_dst'),
		'--file' => "$tempdir/clean.sql",
	],
	'plain clean dump executes on an existing schema');
is($node->safe_psql('dump_dst', 'SELECT * FROM a ORDER BY id'),
	"1|1\n2|2", 'partition data restored');
is($node->safe_psql('dump_dst', 'SELECT * FROM b'),
	'3|1', 'multilevel partition data restored');
is( $node->safe_psql(
		'dump_dst',
		q{SELECT c.conname, c.conparentid = 0, p.relname
          FROM pg_constraint c
          JOIN pg_inherits i ON i.inhrelid = c.conindid
          JOIN pg_class p ON p.oid = i.inhparent
          WHERE c.conname IN ('a1_key', 'b11_pkey')
          ORDER BY c.conname}),
	"a1_key|t|a_idx\nb11_pkey|t|b1_idx",
	'local constraints and index attachments restored');

# The child indexes need not be included in a selective dump.  Inspect the
# entire index hierarchy, rather than just indexes selected for dumping.
$node->command_ok(
	[
		'pg_dump', '--clean', '--schema-only', '--no-sync',
		'--table=a', '--table=b',
		'--file' => "$tempdir/parents.sql",
		'--dbname' => $node->connstr('dump_src'),
	],
	'clean dump of parent tables only');
my $parents = slurp_file("$tempdir/parents.sql");
unlike(
	$parents,
	qr/CREATE TABLE public\.(?:a1|b1)\b/,
	'child tables excluded from selective dump');
unlike(
	$parents,
	qr/DROP INDEX public\.[ab]_idx;/,
	'selective dump still omits blocked parent index DROPs');
like(
	$parents,
	qr/CREATE UNIQUE INDEX a_idx ON ONLY public\.a /,
	'selective dump retains parent index creation');

# Selecting just the parent index must not expand the cleanup with CASCADE.
$node->command_ok(
	[
		'pg_restore', '--list',
		'--file' => "$tempdir/toc.list",
		"$tempdir/partition.dump",
	],
	'list partition index archive');
my @toc = split /\n/, slurp_file("$tempdir/toc.list");
my @indexes = grep { / INDEX public [ac]_idx / } @toc;
is(scalar @indexes, 2, 'parent indexes have archive entries');
append_to_file("$tempdir/indexes.list", join("\n", @indexes) . "\n");
$node->command_fails_like(
	[
		'pg_restore', '--clean',
		'--use-list' => "$tempdir/indexes.list",
		'--file' => "$tempdir/indexes.sql",
		"$tempdir/partition.dump",
	],
	qr/cannot clean "a_idx" without cleaning its table or a covering parent index or constraint/,
	'reject uncovered parent index when generating SQL');

# Include an independent index to prove that failure precedes every DROP.
my $index_oids = $node->safe_psql('dump_dst',
	q{SELECT oid, relname FROM pg_class WHERE relname IN ('a_idx', 'c_idx') ORDER BY oid}
);
$node->command_fails_like(
	[
		'pg_restore', '--clean', '--if-exists',
		'--use-list' => "$tempdir/indexes.list",
		'--dbname' => $node->connstr('dump_dst'),
		"$tempdir/partition.dump",
	],
	qr/cannot clean "a_idx" without cleaning its table or a covering parent index or constraint/,
	'reject uncovered parent index before executing SQL');
is( $node->safe_psql(
		'dump_dst',
		q{SELECT oid, relname FROM pg_class WHERE relname IN ('a_idx', 'c_idx') ORDER BY oid}
	),
	$index_oids,
	'failed planning leaves both selected indexes intact');
is( $node->safe_psql(
		'dump_dst',
		q{SELECT count(*) FROM pg_constraint
          WHERE conrelid = 'refs'::regclass AND contype = 'f'}),
	'1',
	'unselected foreign key is preserved');

my @hierarchy =
  grep { / (?:INDEX|INDEX ATTACH) public d(?:1|11)?_idx / } @toc;
append_to_file("$tempdir/hierarchy.list", join("\n", @hierarchy) . "\n");
for my $option ('--exit-on-error', '--jobs=2')
{
	$node->command_ok(
		[
			'pg_restore', '--clean', $option,
			'--use-list' => "$tempdir/hierarchy.list",
			'--dbname' => $node->connstr('dump_dst'),
			"$tempdir/partition.dump",
		],
		"root index covers multilevel selective cleanup with $option");
}
is( $node->safe_psql(
		'dump_dst',
		q{SELECT inhrelid::regclass, inhparent::regclass FROM pg_inherits
	  WHERE inhrelid IN ('d1_idx'::regclass, 'd11_idx'::regclass)
	  ORDER BY inhrelid::regclass::text}),
	"d11_idx|d1_idx\nd1_idx|d_idx",
	'selective index restore preserves both attachment levels');
is($node->safe_psql('dump_dst', 'SELECT * FROM d'),
	'4|1', 'selective index cleanup preserves table data');

# Selecting two forbidden DROP entries does not provide coverage for either.
my @uncovered = grep { / INDEX public (?:d1_idx|d11_idx|c_idx) / } @toc;
append_to_file("$tempdir/uncovered.list", join("\n", @uncovered) . "\n");
$node->command_fails_like(
	[
		'pg_restore', '--clean',
		'--use-list' => "$tempdir/uncovered.list",
		'--dbname' => $node->connstr('dump_dst'),
		"$tempdir/partition.dump",
	],
	qr/cannot clean "d(?:1|11)_idx" without cleaning its table or a covering parent index or constraint/,
	'an uncleaned intermediate index cannot cover its child');
is( $node->safe_psql(
		'dump_dst',
		q{SELECT oid, relname FROM pg_class WHERE relname IN ('a_idx', 'c_idx') ORDER BY oid}
	),
	$index_oids,
	'uncovered multilevel selection leaves independent index intact');

done_testing();
