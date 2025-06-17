
# Copyright (c) 2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');

$node->init;
$node->start;
$node->safe_psql('postgres', "
	CREATE SCHEMA sch1;
	CREATE SCHEMA sch2;

	CREATE TYPE public.aga AS (x integer);

	CREATE FUNCTION sch1.cmp(i public.aga, j public.aga) RETURNS boolean
	LANGUAGE sql IMMUTABLE
	AS \$\$
		SELECT \$1.x < \$2.x;
	\$\$;

	CREATE FUNCTION sch2.cmp(i public.aga, j public.aga) RETURNS boolean
	LANGUAGE sql IMMUTABLE
	AS \$\$
		SELECT \$1.x > \$2.x;
	\$\$;

	CREATE OPERATOR sch1.= (
		FUNCTION = sch1.cmp,
		LEFTARG = public.aga,
		RIGHTARG = public.aga,
		COMMUTATOR = OPERATOR(sch1.=)
	);

	CREATE OPERATOR sch2.= (
		FUNCTION = sch2.cmp,
		LEFTARG = public.aga,
		RIGHTARG = public.aga,
		COMMUTATOR = OPERATOR(sch2.=)
	);

	SET search_path = sch1;
	CREATE TABLE public.tab1 (
			v public.aga,
			g integer GENERATED ALWAYS AS (
		CASE v
			WHEN ROW(1)::public.aga THEN 1
			WHEN ROW(2)::public.aga THEN 2
			ELSE NULL::integer
		END) STORED,
			h integer GENERATED ALWAYS AS (
		CASE
			WHEN (v OPERATOR(sch1.=) ROW(1)::public.aga) THEN 1
			WHEN (v OPERATOR(sch1.=) ROW(2)::public.aga) THEN 2
			ELSE NULL::integer
		END) STORED
	);
	INSERT INTO public.tab1(v) VALUES (ROW(0)), (ROW(1)), (ROW(2)), (ROW(3));

	SET search_path = sch2;
	CREATE TABLE public.tab2 (
			v public.aga,
			g integer GENERATED ALWAYS AS (
		CASE v
			WHEN ROW(1)::public.aga THEN 1
			WHEN ROW(2)::public.aga THEN 2
			ELSE NULL::integer
		END) STORED,
			h integer GENERATED ALWAYS AS (
		CASE
			WHEN (v OPERATOR(sch2.=) ROW(1)::public.aga) THEN 1
			WHEN (v OPERATOR(sch2.=) ROW(2)::public.aga) THEN 2
			ELSE NULL::integer
		END) STORED
	);
	INSERT INTO public.tab2(v) VALUES (ROW(0)), (ROW(1)), (ROW(2)), (ROW(3));
");

my $backup = $node->backup_dir."/dump1";

$node->command_ok(
	[ 'pg_dump', '-f', $backup, $node->connstr('postgres') ]);

my $res = $node->run_log(
	[ 'createdb', 'foo' ]);
ok($res, 'no created errors' );

$res = $node->run_log(
	[ 'psql', '-d', 'foo', '-f', $backup ]);
ok($res, 'no restore errors' );

# before
my $res1 = $node->safe_psql('postgres', "TABLE public.tab1 ORDER BY 1");
my $desc1 = $node->safe_psql('postgres',
q{
	SELECT adbin FROM pg_attrdef
	WHERE adrelid = (SELECT oid FROM pg_class WHERE relname = 'tab1')
});

note("postgres tab1:\n$res1\n");
note("postgres attrdef tab1:\n$desc1\n");

my $res2 = $node->safe_psql('postgres', "TABLE public.tab2 ORDER BY 1");
my $desc2 = $node->safe_psql('postgres',
q{
	SELECT adbin FROM pg_attrdef
	WHERE adrelid = (SELECT oid FROM pg_class WHERE relname = 'tab2')
});

note("postgres tab2:\n$res2\n");
note("postgres attrdef tab2:\n$desc2\n");

# after
my $res3 = $node->safe_psql('foo', "TABLE public.tab1 ORDER BY 1");
my $desc3 = $node->safe_psql('foo',
q{
	SELECT adbin FROM pg_attrdef
	WHERE adrelid = (SELECT oid FROM pg_class WHERE relname = 'tab1')
});

note("foo tab1:\n$res3\n");
note("foo attrdef tab1:\n$desc3\n");

my $res4 = $node->safe_psql('foo', "TABLE public.tab2 ORDER BY 1");
my $desc4 = $node->safe_psql('foo',
q{
	SELECT adbin FROM pg_attrdef
	WHERE adrelid = (SELECT oid FROM pg_class WHERE relname = 'tab2')
});

note("foo tab2:\n$res4\n");
note("foo attrdef tab2:\n$desc4\n");

#
# The content of the tables should be consistent before and after "upgrade".
#
ok($res1 eq $res3, 'contents matches for tab1');
ok($res2 eq $res4, 'contents matches for tab2');

#
# Both operators should provide the same results.
#
sub check_op_eq
{
	my ($relname) = @_;

	return qq{
		WITH tmp AS (SELECT * FROM $relname
					 WHERE g IS NOT NULL OR h IS NOT NULL)
		SELECT COUNT(*)
		FROM (TABLE tmp) a
		FULL JOIN (TABLE tmp) b USING (v)
		WHERE a.g IS NULL OR b.g IS NULL OR a.h IS NULL OR b.h IS NULL;
	};
}

is($node->safe_psql('postgres', check_op_eq('tab1')), 0,
	"no difference for tab1 in database postgres");
is($node->safe_psql('postgres', check_op_eq('tab2')), 0,
	"no difference for tab2 in database postgres");
is($node->safe_psql('foo', check_op_eq('tab1')), 0,
	"no difference for tab1 in database foo");
is($node->safe_psql('foo', check_op_eq('tab2')), 0,
	"no difference for tab2 in database foo");

$node->stop;

done_testing();
