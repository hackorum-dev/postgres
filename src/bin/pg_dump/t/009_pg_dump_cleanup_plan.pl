# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use IPC::Run qw(run timeout);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE dump_src');
$node->safe_psql(
	'dump_src',
	q{
CREATE TABLE z_parent (f1 integer CONSTRAINT positive CHECK (f1 > 0) NOT ENFORCED);
CREATE TABLE a_child (f1 integer);
INSERT INTO a_child VALUES (-1);
ALTER TABLE a_child ADD CONSTRAINT positive CHECK (f1 > 0) NOT VALID;
ALTER TABLE a_child INHERIT z_parent;

-- Two inherited definitions must both be removed before dropping the local one.
CREATE TABLE m_left (f1 integer CONSTRAINT shared CHECK (f1 > 0) NOT ENFORCED);
CREATE TABLE n_right (f1 integer CONSTRAINT shared CHECK (f1 > 0) NOT ENFORCED);
CREATE TABLE b_leaf (f1 integer);
INSERT INTO b_leaf VALUES (-2);
ALTER TABLE b_leaf ADD CONSTRAINT shared CHECK (f1 > 0) NOT VALID;
ALTER TABLE b_leaf INHERIT m_left;
ALTER TABLE b_leaf INHERIT n_right;

-- A purely inherited intermediate constraint has no independent TOC entry.
CREATE TABLE y_root (f1 integer CONSTRAINT deep CHECK (f1 > 0) NOT ENFORCED);
CREATE TABLE x_middle () INHERITS (y_root);
CREATE TABLE c_leaf (f1 integer);
INSERT INTO c_leaf VALUES (-3);
ALTER TABLE c_leaf ADD CONSTRAINT deep CHECK (f1 > 0) NOT VALID;
ALTER TABLE c_leaf INHERIT x_middle;

-- A merged CHECK on an intermediate table must enforce its descendants while
-- leaving existing violating rows visible when NOT VALID was requested.
CREATE TABLE merge_root (f1 integer CONSTRAINT merged CHECK (f1 > 0) NOT ENFORCED);
CREATE TABLE merge_middle () INHERITS (merge_root);
CREATE TABLE merge_leaf () INHERITS (merge_middle);
INSERT INTO merge_leaf VALUES (-4);
ALTER TABLE merge_middle ADD CONSTRAINT merged CHECK (f1 > 0) NOT VALID;

-- NOT NULL inherits by column name.  Names and attribute numbers differ, and
-- the local leaf inherits through a purely inherited intermediate definition.
-- The original direct-parent case also uses identical constraint names.
CREATE TABLE a_nn_direct (f1 integer);
ALTER TABLE a_nn_direct ADD CONSTRAINT nn_direct NOT NULL f1 NOT VALID;
CREATE TABLE z_nn_direct (f1 integer);
ALTER TABLE z_nn_direct ADD CONSTRAINT nn_direct NOT NULL f1 NOT VALID;
ALTER TABLE z_nn_direct INHERIT a_nn_direct;

CREATE TABLE a_nn_parent (f1 integer);
CREATE TABLE m_nn_middle () INHERITS (a_nn_parent);
CREATE TABLE n_nn_middle () INHERITS (a_nn_parent);
ALTER TABLE a_nn_parent ADD CONSTRAINT nn_parent NOT NULL f1 NOT VALID;
CREATE TABLE b_nn_other (f1 integer);
ALTER TABLE b_nn_other ADD CONSTRAINT nn_other NOT NULL f1 NOT VALID;
CREATE TABLE c_nn_noinh (f1 integer);
ALTER TABLE c_nn_noinh ADD CONSTRAINT nn_stop NOT NULL f1 NOT VALID NO INHERIT;
CREATE TABLE z_nn_child (extra integer, f1 integer);
INSERT INTO z_nn_child VALUES (1, NULL);
ALTER TABLE z_nn_child ADD CONSTRAINT nn_child NOT NULL f1 NOT VALID;
ALTER TABLE z_nn_child INHERIT m_nn_middle;
ALTER TABLE z_nn_child INHERIT n_nn_middle;
ALTER TABLE z_nn_child INHERIT b_nn_other;
ALTER TABLE z_nn_child INHERIT c_nn_noinh;

CREATE TABLE unrelated (f1 integer);
ALTER TABLE unrelated ADD CONSTRAINT keep_me CHECK (f1 > 0) NOT VALID;
});

my $catalog_query = q{
SELECT conrelid::regclass, conname, conislocal, coninhcount, conenforced, convalidated
FROM pg_constraint
WHERE contype IN ('c', 'n') AND connamespace = 'public'::regnamespace
ORDER BY conrelid::regclass::text, conname
};
my $source_catalog = $node->safe_psql('dump_src', $catalog_query);
like(
	$source_catalog,
	qr/a_child\|positive\|t\|1\|t\|f/,
	'local inherited CHECK starts enforced and unvalidated');
like(
	$source_catalog,
	qr/merge_leaf\|merged\|f\|1\|t\|f/,
	'merged CHECK enforcement reaches an unvalidated descendant');
like(
	$source_catalog,
	qr/z_nn_child\|nn_child\|t\|3\|t\|f/,
	'local NOT NULL inherits from three parents, excluding NO INHERIT');

$node->command_ok(
	[
		'pg_dump', '--clean', '--no-sync', '--no-owner', '--no-acl',
		'--file' => "$tempdir/clean.sql",
		'--dbname' => $node->connstr('dump_src')
	],
	'generate plain clean dump');
my $plain = slurp_file("$tempdir/clean.sql");
unlike(
	$plain,
	qr/ALTER TABLE public\.a_child DROP CONSTRAINT positive;/,
	'table cleanup covers inherited local CHECK');
unlike(
	$plain,
	qr/ALTER TABLE public\.z_nn_child DROP CONSTRAINT nn_child;/,
	'table cleanup covers inherited local NOT NULL');
$node->command_ok(
	[
		'psql', '-X', '-v', 'ON_ERROR_STOP=1',
		'--dbname' => $node->connstr('dump_src'),
		'--file' => "$tempdir/clean.sql"
	],
	'plain clean restore succeeds with violating old rows');
is($node->safe_psql('dump_src', $catalog_query),
	$source_catalog, 'plain restore preserves constraint lifecycle facts');

my $custom;
for my $format ('custom', 'directory', 'tar')
{
	my $archive = "$tempdir/$format.dump";
	my $dbname = "dst_$format";
	$custom = $archive if $format eq 'custom';
	$node->safe_psql('postgres', "CREATE DATABASE $dbname");
	$node->command_ok(
		[
			'pg_dump', "--format=$format", '--no-sync',
			'--file' => $archive,
			'--dbname' => $node->connstr('dump_src')
		],
		"dump $format archive with cleanup metadata");
	$node->command_ok(
		[
			'pg_restore', '--exit-on-error',
			'--dbname' => $node->connstr($dbname),
			$archive
		],
		"initial $format restore");
	for my $option ('--exit-on-error', '--single-transaction',
		'--transaction-size=2')
	{
		$node->command_ok(
			[
				'pg_restore', '--clean', '--if-exists', $option,
				'--dbname' => $node->connstr($dbname),
				$archive
			],
			"$format clean restore with $option");
	}
	is( $node->safe_psql($dbname, $catalog_query),
		$source_catalog,
		"$format restore preserves local, inherited, enforced and validated facts"
	);
	is( $node->safe_psql(
			$dbname,
			'SET constraint_exclusion=on; SELECT f1 FROM a_child WHERE f1 < 0'
		),
		'-1',
		"$format restore does not hide rows using an invalid CHECK");
}

# Put parent CHECKs before local children in creation order, so reverse TOC
# alone would drop still-inherited children first.  Cleanup must reorder DROPs
# while preserving this supported creation order.
$node->command_ok(
	[ 'pg_restore', '--list', '--file' => "$tempdir/toc.list", $custom ],
	'list archive');
my @toc = split /\n/, slurp_file("$tempdir/toc.list");
my @checks =
  grep {
	/ CHECK CONSTRAINT public / && !/ unrelated keep_me / && !/ public merge_/
  } @toc;
@checks =
  sort { ($b =~ / public (\S+) /)[0] cmp($a =~ / public (\S+) /)[0] } @checks;
append_to_file("$tempdir/checks.list", join("\n", @checks) . "\n");
$node->command_ok(
	[
		'pg_restore', '--clean',
		'--use-list' => "$tempdir/checks.list",
		'--file' => "$tempdir/checks.sql",
		$custom
	],
	'plan selective CHECK cleanup');
my $selective = slurp_file("$tempdir/checks.sql");
like(
	$selective,
	qr/ALTER TABLE public\.z_parent DROP CONSTRAINT positive;.*ALTER TABLE public\.a_child DROP CONSTRAINT positive;/s,
	'parent cleanup precedes local inherited CHECK cleanup');

for my $parent ('m_left', 'n_right')
{
	like(
		$selective,
		qr/ALTER TABLE public\.$parent DROP CONSTRAINT shared;.*ALTER TABLE public\.b_leaf DROP CONSTRAINT shared;/s,
		"$parent is cleaned before the multiply inherited CHECK");
}
like(
	$selective,
	qr/ALTER TABLE public\.y_root DROP CONSTRAINT deep;.*ALTER TABLE public\.c_leaf DROP CONSTRAINT deep;/s,
	'cleanup crosses a purely inherited intermediate definition');
like(
	$selective,
	qr/ALTER TABLE public\.z_parent\n.*ALTER TABLE public\.a_child\n/s,
	'cleanup ordering does not reorder creation');
$node->command_ok(
	[
		'pg_restore', '--clean', '--exit-on-error',
		'--use-list' => "$tempdir/checks.list",
		'--dbname' => $node->connstr('dst_custom'),
		$custom
	],
	'selective clean restore of CHECKs without dropping tables');
is( $node->safe_psql('dst_custom', $catalog_query),
	$source_catalog,
	'selective restore preserves lifecycle facts and unselected constraints');

# Missing cleanup parents must be detected before cleaning unrelated entries.
my @incomplete =
  grep { / CHECK CONSTRAINT public (?:a_child positive|unrelated keep_me) / }
  @toc;
append_to_file("$tempdir/incomplete.list", join("\n", @incomplete) . "\n");
$node->command_fails_like(
	[
		'pg_restore', '--clean', '--exit-on-error',
		'--use-list' => "$tempdir/incomplete.list",
		'--dbname' => $node->connstr('dst_custom'),
		$custom
	],
	qr/cannot clean "a_child positive".*Cleanup entry "z_parent positive" \(DumpId \d+\) is not selected for cleanup/s,
	'reject incomplete cleanup selection before issuing any DROP');
is($node->safe_psql('dst_custom', $catalog_query),
	$source_catalog,
	'failed cleanup planning leaves all constraints unchanged');

# Select only local NOT NULL definitions, putting the parents first in creation
# order so reverse TOC alone would try to drop the still-inherited child.
my @notnulls = grep {
	/ CONSTRAINT public (?:a_nn_parent nn_parent|b_nn_other nn_other|z_nn_child nn_child|[az]_nn_direct nn_direct) /
} @toc;
@notnulls =
  sort { ($a =~ / public (\S+) /)[0] cmp($b =~ / public (\S+) /)[0] }
  @notnulls;
append_to_file("$tempdir/notnull.list", join("\n", @notnulls) . "\n");
is(scalar @notnulls, 5, 'select five independent NOT NULL definitions');
$node->command_ok(
	[
		'pg_restore', '--clean',
		'--use-list' => "$tempdir/notnull.list",
		'--file' => "$tempdir/notnull.sql",
		$custom
	],
	'plan selective NOT NULL cleanup');
my $nn_sql = slurp_file("$tempdir/notnull.sql");
for my $parent ('a_nn_parent', 'b_nn_other')
{
	like(
		$nn_sql,
		qr/ALTER TABLE public\.$parent DROP CONSTRAINT .*ALTER TABLE public\.z_nn_child DROP CONSTRAINT nn_child;/s,
		"$parent cleanup precedes local inherited NOT NULL cleanup");
}
# Different NOT NULL names require creating the local child before inherited
# definitions choose a name for it.  Keep the direct same-name pair parent-first
# so its actual restore still requires reordering cleanup rather than creation.
my @nn_restore = (
	(grep { / z_nn_child nn_child / } @notnulls),
	(grep { !/ z_nn_child nn_child / } @notnulls));
append_to_file("$tempdir/notnull-restore.list",
	join("\n", @nn_restore) . "\n");
$node->command_ok(
	[
		'pg_restore', '--clean', '--exit-on-error',
		'--use-list' => "$tempdir/notnull-restore.list",
		'--dbname' => $node->connstr('dst_custom'),
		$custom
	],
	'selective clean restore of NOT NULL without dropping tables');
is($node->safe_psql('dst_custom', $catalog_query),
	$source_catalog,
	'selective NOT NULL restore preserves inheritance and validity');
is( $node->safe_psql(
		'dst_custom', 'SELECT count(*) FROM z_nn_child WHERE f1 IS NULL'),
	'1',
	'NOT VALID restore preserves existing null rows');
my @nn_incomplete = grep { !/ b_nn_other nn_other / } @notnulls;
push @nn_incomplete,
  grep { / CHECK CONSTRAINT public unrelated keep_me / } @toc;
append_to_file("$tempdir/notnull-incomplete.list",
	join("\n", @nn_incomplete) . "\n");
my $nn_oids =
  q{SELECT oid FROM pg_constraint WHERE connamespace = 'public'::regnamespace ORDER BY oid};
my $before_nn = $node->safe_psql('dst_custom', $nn_oids);
$node->command_fails_like(
	[
		'pg_restore', '--clean', '--exit-on-error',
		'--use-list' => "$tempdir/notnull-incomplete.list",
		'--dbname' => $node->connstr('dst_custom'),
		$custom
	],
	qr/cannot clean "z_nn_child nn_child".*Cleanup entry "b_nn_other nn_other" \(DumpId \d+\) is not selected for cleanup/s,
	'missing NOT NULL parent is rejected before any DROP');
is($node->safe_psql('dst_custom', $nn_oids),
	$before_nn,
	'incomplete NOT NULL cleanup leaves all constraint OIDs unchanged');

# Each of two tables per layer inherits both tables in the preceding layer.
# There are only 67 tables, but exponentially many paths to the two roots.
# Keep this separate from the format matrix and exercise the CHECK collector
# on a schema-only dump, without relying on a tight benchmark time limit.
$node->safe_psql('postgres', 'CREATE DATABASE diamond');
my $diamond_sql = q{
CREATE TABLE diamond_root_l (f1 integer CONSTRAINT diamond_check CHECK (f1 > 0) NOT ENFORCED);
CREATE TABLE diamond_root_r (f1 integer CONSTRAINT diamond_check CHECK (f1 > 0) NOT ENFORCED);
};
my @parents = ('diamond_root_l', 'diamond_root_r');
for my $level (1 .. 32)
{
	my @next;
	for my $side ('l', 'r')
	{
		my $table = sprintf('diamond_%s_%02d', $side, $level);
		$diamond_sql .= sprintf("CREATE TABLE %s () INHERITS (%s);\n",
			$table, join(', ', @parents));
		push @next, $table;
	}
	@parents = @next;
}
$diamond_sql .= q{
CREATE TABLE diamond_leaf (f1 integer);
INSERT INTO diamond_leaf VALUES (-4);
ALTER TABLE diamond_leaf ADD CONSTRAINT diamond_check CHECK (f1 > 0) NOT VALID;
};
$diamond_sql .= sprintf("ALTER TABLE diamond_leaf INHERIT %s;\n", $_)
  for @parents;
$node->safe_psql('diamond', $diamond_sql);
my $diamond_catalog = $node->safe_psql('diamond', $catalog_query);
my ($stdout, $stderr);
my $dump_ok = eval {
	run [
		'pg_dump', '--schema-only', '--format=custom', '--no-sync',
		'--file' => "$tempdir/diamond.dump",
		'--dbname' => $node->connstr('diamond')
	  ],
	  '>' => \$stdout,
	  '2>' => \$stderr,
	  timeout($PostgreSQL::Test::Utils::timeout_default);
};
ok($dump_ok,
	'dump shared CHECK ancestry within the standard command timeout');
BAIL_OUT("diamond dump failed: $@ $stderr") unless $dump_ok;
$node->command_ok(
	[
		'pg_restore', '--list',
		'--file' => "$tempdir/diamond.list",
		"$tempdir/diamond.dump"
	],
	'list diamond CHECK archive');
my @diamond_toc = split /\n/, slurp_file("$tempdir/diamond.list");
my @diamond_checks = grep {
	/ CHECK CONSTRAINT public (?:diamond_root_[lr]|diamond_leaf) diamond_check /
} @diamond_toc;
is(scalar @diamond_checks,
	3, 'only the two roots and local leaf need CHECK entries');
append_to_file("$tempdir/diamond-checks.list",
	join("\n", @diamond_checks) . "\n");
$node->command_ok(
	[
		'pg_restore', '--clean',
		'--use-list' => "$tempdir/diamond-checks.list",
		'--file' => "$tempdir/diamond-checks.sql",
		"$tempdir/diamond.dump"
	],
	'plan cleanup through shared CHECK ancestors');
my $diamond_clean = slurp_file("$tempdir/diamond-checks.sql");
for my $side ('l', 'r')
{
	like(
		$diamond_clean,
		qr/ALTER TABLE public\.diamond_root_$side DROP CONSTRAINT diamond_check;.*ALTER TABLE public\.diamond_leaf DROP CONSTRAINT diamond_check;/s,
		"shared ancestry retains cleanup of root $side before the leaf");
}
$node->command_ok(
	[
		'pg_restore', '--clean', '--exit-on-error',
		'--use-list' => "$tempdir/diamond-checks.list",
		'--dbname' => $node->connstr('diamond'),
		"$tempdir/diamond.dump"
	],
	'selective CHECK restore through 32 shared inheritance layers');
is($node->safe_psql('diamond', $catalog_query),
	$diamond_catalog,
	'shared ancestry restore preserves all constraint lifecycle facts');
is( $node->safe_psql(
		'diamond',
		'SET constraint_exclusion=on; SELECT f1 FROM diamond_leaf WHERE f1 < 0'
	),
	'-4',
	'shared ancestry restore retains violating old data');

my @diamond_incomplete =
  grep { !/ diamond_root_r diamond_check / } @diamond_checks;
append_to_file(
	"$tempdir/diamond-incomplete.list",
	join("\n", @diamond_incomplete) . "\n");
$node->command_fails_like(
	[
		'pg_restore', '--clean',
		'--use-list' => "$tempdir/diamond-incomplete.list",
		'--dbname' => $node->connstr('diamond'),
		"$tempdir/diamond.dump"
	],
	qr/cannot clean "diamond_leaf diamond_check".*Cleanup entry "diamond_root_r diamond_check" \(DumpId \d+\) is not selected for cleanup/s,
	'shared ancestry still requires both local root definitions');
is($node->safe_psql('diamond', $catalog_query),
	$diamond_catalog,
	'incomplete shared ancestry cleanup leaves all constraints intact');

done_testing();
