--
-- SUBSELECT
--

SELECT 1 AS one WHERE 1 IN (SELECT 1);

SELECT 1 AS zero WHERE 1 NOT IN (SELECT 1);

SELECT 1 AS zero WHERE 1 IN (SELECT 2);

-- Check grammar's handling of extra parens in assorted contexts

SELECT * FROM (SELECT 1 AS x) ss;
SELECT * FROM ((SELECT 1 AS x)) ss;

(SELECT 2) UNION SELECT 2;
((SELECT 2)) UNION SELECT 2;

SELECT ((SELECT 2) UNION SELECT 2);
SELECT (((SELECT 2)) UNION SELECT 2);

SELECT (SELECT ARRAY[1,2,3])[1];
SELECT ((SELECT ARRAY[1,2,3]))[2];
SELECT (((SELECT ARRAY[1,2,3])))[3];

-- Set up some simple test tables

CREATE TABLE SUBSELECT_TBL (
  f1 integer,
  f2 integer,
  f3 float
);

INSERT INTO SUBSELECT_TBL VALUES (1, 2, 3);
INSERT INTO SUBSELECT_TBL VALUES (2, 3, 4);
INSERT INTO SUBSELECT_TBL VALUES (3, 4, 5);
INSERT INTO SUBSELECT_TBL VALUES (1, 1, 1);
INSERT INTO SUBSELECT_TBL VALUES (2, 2, 2);
INSERT INTO SUBSELECT_TBL VALUES (3, 3, 3);
INSERT INTO SUBSELECT_TBL VALUES (6, 7, 8);
INSERT INTO SUBSELECT_TBL VALUES (8, 9, NULL);

SELECT '' AS eight, * FROM SUBSELECT_TBL;

-- Uncorrelated subselects

SELECT '' AS two, f1 AS "Constant Select" FROM SUBSELECT_TBL
  WHERE f1 IN (SELECT 1);

SELECT '' AS six, f1 AS "Uncorrelated Field" FROM SUBSELECT_TBL
  WHERE f1 IN (SELECT f2 FROM SUBSELECT_TBL);

SELECT '' AS six, f1 AS "Uncorrelated Field" FROM SUBSELECT_TBL
  WHERE f1 IN (SELECT f2 FROM SUBSELECT_TBL WHERE
    f2 IN (SELECT f1 FROM SUBSELECT_TBL));

SELECT '' AS three, f1, f2
  FROM SUBSELECT_TBL
  WHERE (f1, f2) NOT IN (SELECT f2, CAST(f3 AS int4) FROM SUBSELECT_TBL
                         WHERE f3 IS NOT NULL);

-- Correlated subselects

SELECT '' AS six, f1 AS "Correlated Field", f2 AS "Second Field"
  FROM SUBSELECT_TBL upper
  WHERE f1 IN (SELECT f2 FROM SUBSELECT_TBL WHERE f1 = upper.f1);

SELECT '' AS six, f1 AS "Correlated Field", f3 AS "Second Field"
  FROM SUBSELECT_TBL upper
  WHERE f1 IN
    (SELECT f2 FROM SUBSELECT_TBL WHERE CAST(upper.f2 AS float) = f3);

SELECT '' AS six, f1 AS "Correlated Field", f3 AS "Second Field"
  FROM SUBSELECT_TBL upper
  WHERE f3 IN (SELECT upper.f1 + f2 FROM SUBSELECT_TBL
               WHERE f2 = CAST(f3 AS integer));

SELECT '' AS five, f1 AS "Correlated Field"
  FROM SUBSELECT_TBL
  WHERE (f1, f2) IN (SELECT f2, CAST(f3 AS int4) FROM SUBSELECT_TBL
                     WHERE f3 IS NOT NULL);

--
-- Use some existing tables in the regression test
--

SELECT '' AS eight, ss.f1 AS "Correlated Field", ss.f3 AS "Second Field"
  FROM SUBSELECT_TBL ss
  WHERE f1 NOT IN (SELECT f1+1 FROM INT4_TBL
                   WHERE f1 != ss.f1 AND f1 < 2147483647);

select q1, float8(count(*)) / (select count(*) from int8_tbl)
from int8_tbl group by q1 order by q1;

-- Unspecified-type literals in output columns should resolve as text

SELECT *, pg_typeof(f1) FROM
  (SELECT 'foo' AS f1 FROM generate_series(1,3)) ss ORDER BY 1;

-- ... unless there's context to suggest differently

explain (verbose, costs off) select '42' union all select '43';
explain (verbose, costs off) select '42' union all select 43;

-- check materialization of an initplan reference (bug #14524)
explain (verbose, costs off)
select 1 = all (select (select 1));
select 1 = all (select (select 1));

--
-- Check EXISTS simplification with LIMIT
--
explain (costs off)
select * from int4_tbl o where exists
  (select 1 from int4_tbl i where i.f1=o.f1 limit null);
explain (costs off)
select * from int4_tbl o where not exists
  (select 1 from int4_tbl i where i.f1=o.f1 limit 1);
explain (costs off)
select * from int4_tbl o where exists
  (select 1 from int4_tbl i where i.f1=o.f1 limit 0);

--
-- Test cases to catch unpleasant interactions between IN-join processing
-- and subquery pullup.
--

select count(*) from
  (select 1 from tenk1 a
   where unique1 IN (select hundred from tenk1 b)) ss;
select count(distinct ss.ten) from
  (select ten from tenk1 a
   where unique1 IN (select hundred from tenk1 b)) ss;
select count(*) from
  (select 1 from tenk1 a
   where unique1 IN (select distinct hundred from tenk1 b)) ss;
select count(distinct ss.ten) from
  (select ten from tenk1 a
   where unique1 IN (select distinct hundred from tenk1 b)) ss;

--
-- Test cases to check for overenthusiastic optimization of
-- "IN (SELECT DISTINCT ...)" and related cases.  Per example from
-- Luca Pireddu and Michael Fuhr.
--

CREATE TEMP TABLE foo (id integer);
CREATE TEMP TABLE bar (id1 integer, id2 integer);

INSERT INTO foo VALUES (1);

INSERT INTO bar VALUES (1, 1);
INSERT INTO bar VALUES (2, 2);
INSERT INTO bar VALUES (3, 1);

-- These cases require an extra level of distinct-ing above subquery s
SELECT * FROM foo WHERE id IN
    (SELECT id2 FROM (SELECT DISTINCT id1, id2 FROM bar) AS s);
SELECT * FROM foo WHERE id IN
    (SELECT id2 FROM (SELECT id1,id2 FROM bar GROUP BY id1,id2) AS s);
SELECT * FROM foo WHERE id IN
    (SELECT id2 FROM (SELECT id1, id2 FROM bar UNION
                      SELECT id1, id2 FROM bar) AS s);

-- These cases do not
SELECT * FROM foo WHERE id IN
    (SELECT id2 FROM (SELECT DISTINCT ON (id2) id1, id2 FROM bar) AS s);
SELECT * FROM foo WHERE id IN
    (SELECT id2 FROM (SELECT id2 FROM bar GROUP BY id2) AS s);
SELECT * FROM foo WHERE id IN
    (SELECT id2 FROM (SELECT id2 FROM bar UNION
                      SELECT id2 FROM bar) AS s);

--
-- Test case to catch problems with multiply nested sub-SELECTs not getting
-- recalculated properly.  Per bug report from Didier Moens.
--

CREATE TABLE orderstest (
    approver_ref integer,
    po_ref integer,
    ordercanceled boolean
);

INSERT INTO orderstest VALUES (1, 1, false);
INSERT INTO orderstest VALUES (66, 5, false);
INSERT INTO orderstest VALUES (66, 6, false);
INSERT INTO orderstest VALUES (66, 7, false);
INSERT INTO orderstest VALUES (66, 1, true);
INSERT INTO orderstest VALUES (66, 8, false);
INSERT INTO orderstest VALUES (66, 1, false);
INSERT INTO orderstest VALUES (77, 1, false);
INSERT INTO orderstest VALUES (1, 1, false);
INSERT INTO orderstest VALUES (66, 1, false);
INSERT INTO orderstest VALUES (1, 1, false);

CREATE VIEW orders_view AS
SELECT *,
(SELECT CASE
   WHEN ord.approver_ref=1 THEN '---' ELSE 'Approved'
 END) AS "Approved",
(SELECT CASE
 WHEN ord.ordercanceled
 THEN 'Canceled'
 ELSE
  (SELECT CASE
		WHEN ord.po_ref=1
		THEN
		 (SELECT CASE
				WHEN ord.approver_ref=1
				THEN '---'
				ELSE 'Approved'
			END)
		ELSE 'PO'
	END)
END) AS "Status",
(CASE
 WHEN ord.ordercanceled
 THEN 'Canceled'
 ELSE
  (CASE
		WHEN ord.po_ref=1
		THEN
		 (CASE
				WHEN ord.approver_ref=1
				THEN '---'
				ELSE 'Approved'
			END)
		ELSE 'PO'
	END)
END) AS "Status_OK"
FROM orderstest ord;

SELECT * FROM orders_view;

DROP TABLE orderstest cascade;

--
-- Test cases to catch situations where rule rewriter fails to propagate
-- hasSubLinks flag correctly.  Per example from Kyle Bateman.
--

create temp table parts (
    partnum     text,
    cost        float8
);

create temp table shipped (
    ttype       char(2),
    ordnum      int4,
    partnum     text,
    value       float8
);

create temp view shipped_view as
    select * from shipped where ttype = 'wt';

create rule shipped_view_insert as on insert to shipped_view do instead
    insert into shipped values('wt', new.ordnum, new.partnum, new.value);

insert into parts (partnum, cost) values (1, 1234.56);

insert into shipped_view (ordnum, partnum, value)
    values (0, 1, (select cost from parts where partnum = '1'));

select * from shipped_view;

create rule shipped_view_update as on update to shipped_view do instead
    update shipped set partnum = new.partnum, value = new.value
        where ttype = new.ttype and ordnum = new.ordnum;

update shipped_view set value = 11
    from int4_tbl a join int4_tbl b
      on (a.f1 = (select f1 from int4_tbl c where c.f1=b.f1))
    where ordnum = a.f1;

select * from shipped_view;

select f1, ss1 as relabel from
    (select *, (select sum(f1) from int4_tbl b where f1 >= a.f1) as ss1
     from int4_tbl a) ss;

--
-- Test cases involving PARAM_EXEC parameters and min/max index optimizations.
-- Per bug report from David Sanchez i Gregori.
--

select * from (
  select max(unique1) from tenk1 as a
  where exists (select 1 from tenk1 as b where b.thousand = a.unique2)
) ss;

select * from (
  select min(unique1) from tenk1 as a
  where not exists (select 1 from tenk1 as b where b.unique2 = 10000)
) ss;

--
-- Test that an IN implemented using a UniquePath does unique-ification
-- with the right semantics, as per bug #4113.  (Unfortunately we have
-- no simple way to ensure that this test case actually chooses that type
-- of plan, but it does in releases 7.4-8.3.  Note that an ordering difference
-- here might mean that some other plan type is being used, rendering the test
-- pointless.)
--

create temp table numeric_table (num_col numeric);
insert into numeric_table values (1), (1.000000000000000000001), (2), (3);

create temp table float_table (float_col float8);
insert into float_table values (1), (2), (3);

select * from float_table
  where float_col in (select num_col from numeric_table);

select * from numeric_table
  where num_col in (select float_col from float_table);

--
-- Test case for bug #4290: bogus calculation of subplan param sets
--

create temp table ta (id int primary key, val int);

insert into ta values(1,1);
insert into ta values(2,2);

create temp table tb (id int primary key, aval int);

insert into tb values(1,1);
insert into tb values(2,1);
insert into tb values(3,2);
insert into tb values(4,2);

create temp table tc (id int primary key, aid int);

insert into tc values(1,1);
insert into tc values(2,2);

select
  ( select min(tb.id) from tb
    where tb.aval = (select ta.val from ta where ta.id = tc.aid) ) as min_tb_id
from tc;

--
-- Test case for 8.3 "failed to locate grouping columns" bug
--

create temp table t1 (f1 numeric(14,0), f2 varchar(30));

select * from
  (select distinct f1, f2, (select f2 from t1 x where x.f1 = up.f1) as fs
   from t1 up) ss
group by f1,f2,fs;

--
-- Test case for bug #5514 (mishandling of whole-row Vars in subselects)
--

create temp table table_a(id integer);
insert into table_a values (42);

create temp view view_a as select * from table_a;

select view_a from view_a;
select (select view_a) from view_a;
select (select (select view_a)) from view_a;
select (select (a.*)::text) from view_a a;

--
-- Check that whole-row Vars reading the result of a subselect don't include
-- any junk columns therein
--

select q from (select max(f1) from int4_tbl group by f1 order by f1) q;
with q as (select max(f1) from int4_tbl group by f1 order by f1)
  select q from q;

--
-- Test case for sublinks pulled up into joinaliasvars lists in an
-- inherited update/delete query
--

begin;  --  this shouldn't delete anything, but be safe

delete from road
where exists (
  select 1
  from
    int4_tbl cross join
    ( select f1, array(select q1 from int8_tbl) as arr
      from text_tbl ) ss
  where road.name = ss.f1 );

rollback;

--
-- Test case for sublinks pushed down into subselects via join alias expansion
--

select
  (select sq1) as qq1
from
  (select exists(select 1 from int4_tbl where f1 = q2) as sq1, 42 as dummy
   from int8_tbl) sq0
  join
  int4_tbl i4 on dummy = i4.f1;

--
-- Test case for subselect within UPDATE of INSERT...ON CONFLICT DO UPDATE
--
create temp table upsert(key int4 primary key, val text);
insert into upsert values(1, 'val') on conflict (key) do update set val = 'not seen';
insert into upsert values(1, 'val') on conflict (key) do update set val = 'seen with subselect ' || (select f1 from int4_tbl where f1 != 0 limit 1)::text;

select * from upsert;

with aa as (select 'int4_tbl' u from int4_tbl limit 1)
insert into upsert values (1, 'x'), (999, 'y')
on conflict (key) do update set val = (select u from aa)
returning *;

--
-- Test case for cross-type partial matching in hashed subplan (bug #7597)
--

create temp table outer_7597 (f1 int4, f2 int4);
insert into outer_7597 values (0, 0);
insert into outer_7597 values (1, 0);
insert into outer_7597 values (0, null);
insert into outer_7597 values (1, null);

create temp table inner_7597(c1 int8, c2 int8);
insert into inner_7597 values(0, null);

select * from outer_7597 where (f1, f2) not in (select * from inner_7597);

--
-- Similar test case using text that verifies that collation
-- information is passed through by execTuplesEqual() in nodeSubplan.c
-- (otherwise it would error in texteq())
--

create temp table outer_text (f1 text, f2 text);
insert into outer_text values ('a', 'a');
insert into outer_text values ('b', 'a');
insert into outer_text values ('a', null);
insert into outer_text values ('b', null);

create temp table inner_text (c1 text, c2 text);
insert into inner_text values ('a', null);

select * from outer_text where (f1, f2) not in (select * from inner_text);

--
-- Another test case for cross-type hashed subplans: comparison of
-- inner-side values must be done with appropriate operator
--

explain (verbose, costs off)
select 'foo'::text in (select 'bar'::name union all select 'bar'::name);

select 'foo'::text in (select 'bar'::name union all select 'bar'::name);

--
-- Test case for premature memory release during hashing of subplan output
--

select '1'::text in (select '1'::name union all select '1'::name);

--
-- Test case for planner bug with nested EXISTS handling
--
select a.thousand from tenk1 a, tenk1 b
where a.thousand = b.thousand
  and exists ( select 1 from tenk1 c where b.hundred = c.hundred
                   and not exists ( select 1 from tenk1 d
                                    where a.thousand = d.thousand ) );

--
-- Check that nested sub-selects are not pulled up if they contain volatiles
--
explain (verbose, costs off)
  select x, x from
    (select (select now()) as x from (values(1),(2)) v(y)) ss;
explain (verbose, costs off)
  select x, x from
    (select (select random()) as x from (values(1),(2)) v(y)) ss;
explain (verbose, costs off)
  select x, x from
    (select (select now() where y=y) as x from (values(1),(2)) v(y)) ss;
explain (verbose, costs off)
  select x, x from
    (select (select random() where y=y) as x from (values(1),(2)) v(y)) ss;

--
-- Test rescan of a hashed subplan (the use of random() is to prevent the
-- sub-select from being pulled up, which would result in not hashing)
--
explain (verbose, costs off)
select sum(ss.tst::int) from
  onek o cross join lateral (
  select i.ten in (select f1 from int4_tbl where f1 <= o.hundred) as tst,
         random() as r
  from onek i where i.unique1 = o.unique1 ) ss
where o.ten = 0;

select sum(ss.tst::int) from
  onek o cross join lateral (
  select i.ten in (select f1 from int4_tbl where f1 <= o.hundred) as tst,
         random() as r
  from onek i where i.unique1 = o.unique1 ) ss
where o.ten = 0;

--
-- Test rescan of a SetOp node
--
explain (costs off)
select count(*) from
  onek o cross join lateral (
    select * from onek i1 where i1.unique1 = o.unique1
    except
    select * from onek i2 where i2.unique1 = o.unique2
  ) ss
where o.ten = 1;

select count(*) from
  onek o cross join lateral (
    select * from onek i1 where i1.unique1 = o.unique1
    except
    select * from onek i2 where i2.unique1 = o.unique2
  ) ss
where o.ten = 1;

--
-- Test rescan of a RecursiveUnion node
--
explain (costs off)
select sum(o.four), sum(ss.a) from
  onek o cross join lateral (
    with recursive x(a) as
      (select o.four as a
       union
       select a + 1 from x
       where a < 10)
    select * from x
  ) ss
where o.ten = 1;

select sum(o.four), sum(ss.a) from
  onek o cross join lateral (
    with recursive x(a) as
      (select o.four as a
       union
       select a + 1 from x
       where a < 10)
    select * from x
  ) ss
where o.ten = 1;

--
-- Check we don't misoptimize a NOT IN where the subquery returns no rows.
--
create temp table notinouter (a int);
create temp table notininner (b int not null);
insert into notinouter values (null), (1);

select * from notinouter where a not in (select b from notininner);

--
-- Check we behave sanely in corner case of empty SELECT list (bug #8648)
--
create temp table nocolumns();
select exists(select * from nocolumns);

--
-- Check behavior with a SubPlan in VALUES (bug #14924)
--
select val.x
  from generate_series(1,10) as s(i),
  lateral (
    values ((select s.i + 1)), (s.i + 101)
  ) as val(x)
where s.i < 10 and (select val.x) < 110;

-- another variant of that (bug #16213)
explain (verbose, costs off)
select * from
(values
  (3 not in (select * from (values (1), (2)) ss1)),
  (false)
) ss;

select * from
(values
  (3 not in (select * from (values (1), (2)) ss1)),
  (false)
) ss;

--
-- Check sane behavior with nested IN SubLinks
--
explain (verbose, costs off)
select * from int4_tbl where
  (case when f1 in (select unique1 from tenk1 a) then f1 else null end) in
  (select ten from tenk1 b);
select * from int4_tbl where
  (case when f1 in (select unique1 from tenk1 a) then f1 else null end) in
  (select ten from tenk1 b);

--
-- Check for incorrect optimization when IN subquery contains a SRF
--
explain (verbose, costs off)
select * from int4_tbl o where (f1, f1) in
  (select f1, generate_series(1,50) / 10 g from int4_tbl i group by f1);
select * from int4_tbl o where (f1, f1) in
  (select f1, generate_series(1,50) / 10 g from int4_tbl i group by f1);

--
-- check for over-optimization of whole-row Var referencing an Append plan
--
select (select q from
         (select 1,2,3 where f1 > 0
          union all
          select 4,5,6.0 where f1 <= 0
         ) q )
from int4_tbl;

--
-- Check that volatile quals aren't pushed down past a DISTINCT:
-- nextval() should not be called more than the nominal number of times
--
create temp sequence ts1;

select * from
  (select distinct ten from tenk1) ss
  where ten < 10 + nextval('ts1')
  order by 1;

select nextval('ts1');

--
-- Check that volatile quals aren't pushed down past a set-returning function;
-- while a nonvolatile qual can be, if it doesn't reference the SRF.
--
create function tattle(x int, y int) returns bool
volatile language plpgsql as $$
begin
  raise notice 'x = %, y = %', x, y;
  return x > y;
end$$;

explain (verbose, costs off)
select * from
  (select 9 as x, unnest(array[1,2,3,11,12,13]) as u) ss
  where tattle(x, 8);

select * from
  (select 9 as x, unnest(array[1,2,3,11,12,13]) as u) ss
  where tattle(x, 8);

-- if we pretend it's stable, we get different results:
alter function tattle(x int, y int) stable;

explain (verbose, costs off)
select * from
  (select 9 as x, unnest(array[1,2,3,11,12,13]) as u) ss
  where tattle(x, 8);

select * from
  (select 9 as x, unnest(array[1,2,3,11,12,13]) as u) ss
  where tattle(x, 8);

-- although even a stable qual should not be pushed down if it references SRF
explain (verbose, costs off)
select * from
  (select 9 as x, unnest(array[1,2,3,11,12,13]) as u) ss
  where tattle(x, u);

select * from
  (select 9 as x, unnest(array[1,2,3,11,12,13]) as u) ss
  where tattle(x, u);

drop function tattle(x int, y int);

--
-- Test that LIMIT can be pushed to SORT through a subquery that just projects
-- columns.  We check for that having happened by looking to see if EXPLAIN
-- ANALYZE shows that a top-N sort was used.  We must suppress or filter away
-- all the non-invariant parts of the EXPLAIN ANALYZE output.
--
create table sq_limit (pk int primary key, c1 int, c2 int);
insert into sq_limit values
    (1, 1, 1),
    (2, 2, 2),
    (3, 3, 3),
    (4, 4, 4),
    (5, 1, 1),
    (6, 2, 2),
    (7, 3, 3),
    (8, 4, 4);

create function explain_sq_limit() returns setof text language plpgsql as
$$
declare ln text;
begin
    for ln in
        explain (analyze, summary off, timing off, costs off)
        select * from (select pk,c2 from sq_limit order by c1,pk) as x limit 3
    loop
        ln := regexp_replace(ln, 'Memory: \S*',  'Memory: xxx');
        return next ln;
    end loop;
end;
$$;

select * from explain_sq_limit();

select * from (select pk,c2 from sq_limit order by c1,pk) as x limit 3;

drop function explain_sq_limit();

drop table sq_limit;

--
-- Ensure that backward scan direction isn't propagated into
-- expression subqueries (bug #15336)
--

begin;

declare c1 scroll cursor for
 select * from generate_series(1,4) i
  where i <> all (values (2),(3));

move forward all in c1;
fetch backward all in c1;

commit;

--
-- Tests for CTE inlining behavior
--

-- Basic subquery that can be inlined
explain (verbose, costs off)
with x as (select * from (select f1 from subselect_tbl) ss)
select * from x where f1 = 1;

-- Explicitly request materialization
explain (verbose, costs off)
with x as materialized (select * from (select f1 from subselect_tbl) ss)
select * from x where f1 = 1;

-- Stable functions are safe to inline
explain (verbose, costs off)
with x as (select * from (select f1, now() from subselect_tbl) ss)
select * from x where f1 = 1;

-- Volatile functions prevent inlining
explain (verbose, costs off)
with x as (select * from (select f1, random() from subselect_tbl) ss)
select * from x where f1 = 1;

-- SELECT FOR UPDATE cannot be inlined
explain (verbose, costs off)
with x as (select * from (select f1 from subselect_tbl for update) ss)
select * from x where f1 = 1;

-- Multiply-referenced CTEs are inlined only when requested
explain (verbose, costs off)
with x as (select * from (select f1, now() as n from subselect_tbl) ss)
select * from x, x x2 where x.n = x2.n;

explain (verbose, costs off)
with x as not materialized (select * from (select f1, now() as n from subselect_tbl) ss)
select * from x, x x2 where x.n = x2.n;

-- Multiply-referenced CTEs can't be inlined if they contain outer self-refs
explain (verbose, costs off)
with recursive x(a) as
  ((values ('a'), ('b'))
   union all
   (with z as not materialized (select * from x)
    select z.a || z1.a as a from z cross join z as z1
    where length(z.a || z1.a) < 5))
select * from x;

with recursive x(a) as
  ((values ('a'), ('b'))
   union all
   (with z as not materialized (select * from x)
    select z.a || z1.a as a from z cross join z as z1
    where length(z.a || z1.a) < 5))
select * from x;

explain (verbose, costs off)
with recursive x(a) as
  ((values ('a'), ('b'))
   union all
   (with z as not materialized (select * from x)
    select z.a || z.a as a from z
    where length(z.a || z.a) < 5))
select * from x;

with recursive x(a) as
  ((values ('a'), ('b'))
   union all
   (with z as not materialized (select * from x)
    select z.a || z.a as a from z
    where length(z.a || z.a) < 5))
select * from x;

-- Check handling of outer references
explain (verbose, costs off)
with x as (select * from int4_tbl)
select * from (with y as (select * from x) select * from y) ss;

explain (verbose, costs off)
with x as materialized (select * from int4_tbl)
select * from (with y as (select * from x) select * from y) ss;

-- Ensure that we inline the currect CTE when there are
-- multiple CTEs with the same name
explain (verbose, costs off)
with x as (select 1 as y)
select * from (with x as (select 2 as y) select * from x) ss;

-- Row marks are not pushed into CTEs
explain (verbose, costs off)
with x as (select * from subselect_tbl)
select * from x for update;

CREATE TABLE s (u INTEGER NOT NULL, n INTEGER NULL, nn INTEGER NOT NULL, p VARCHAR(128) NULL);
insert into s (u, n, nn, p)
    select
    generate_series(1,3) as u,
    generate_series(1,3) as n,
    generate_series(1,3) as nn,
    'foo' as p;
insert into s values(1000002, 1000002, 1000002, 'foofoo');
UPDATE s set n = NULL WHERE n = 3;
analyze s;

CREATE TABLE l (u INTEGER NOT NULL, n INTEGER NULL, nn INTEGER NOT NULL, p VARCHAR(128) NULL);
insert into l (u, n, nn, p)
    select
    generate_series(1,10000 ) as u,
    generate_series(1,10000 ) as n,
    generate_series(1,10000 ) as nn,
    'bar' as p;
UPDATE l set n = NULL WHERE n = 7;

CREATE TABLE s1 (u INTEGER NOT NULL, n INTEGER NULL, n1 INTEGER NULL, nn INTEGER NOT NULL, p VARCHAR(128) NULL);
insert into s1 (u, n, n1, nn, p)
    select
    generate_series(1,3) as u,
    generate_series(1,3) as n,
    generate_series(1,3) as n1,
    generate_series(1,3) as nn,
    'foo' as p;
insert into s1 values(1000003, 1000003, 1000003, 1000003, 'foofoo');
insert into s1 values(1003, 1003, 1003, 1003, 'foofoo');
UPDATE s1 set n = NULL WHERE n = 3;
UPDATE s1 set n1 = NULL WHERE n = 2;
UPDATE s1 set n1 = NULL WHERE n1 = 3;
analyze s1;

CREATE UNIQUE INDEX l_u ON l (u);
CREATE INDEX l_n ON l (n);
CREATE INDEX l_nn ON l (nn);
analyze l;

CREATE TABLE empty (u INTEGER NOT NULL, n INTEGER NULL, nn INTEGER NOT NULL, p VARCHAR(128) NULL);
analyze empty;

-- Recursive CTE
CREATE TABLE employees (
  id serial,
  name varchar(255),
  manager_id int
);

INSERT INTO employees VALUES (1, 'Mark', null);
INSERT INTO employees VALUES (2, 'John', 1);
INSERT INTO employees VALUES (3, 'Dan', 2);
INSERT INTO employees VALUES (4, 'Clark', 1);
INSERT INTO employees VALUES (5, 'Linda', 2);
INSERT INTO employees VALUES (6, 'Willy', 2);
INSERT INTO employees VALUES (7, 'Barack', 2);
INSERT INTO employees VALUES (8, 'Elen', 2);
INSERT INTO employees VALUES (9, 'Kate', 3);
INSERT INTO employees VALUES (10, 'Terry', 4);

WITH RECURSIVE managertree AS (
  SELECT id, name, manager_id
  FROM employees
  WHERE id = 2
  UNION ALL
  SELECT e.id, e.name, e.manager_id
  FROM employees e
  INNER JOIN managertree mtree ON mtree.id = e.manager_id
)
SELECT *
FROM managertree;

--test corrrelated IN subquery
explain (costs off) select count(*) from s where s.u in (select l.u from l where l.n = s.n);

select count(*) from s where s.u in (select l.u from l where l.n = s.n);

set enable_correlated_any_transform = off;

explain (costs off) select count(*) from s where s.u in (select l.u from l where l.n = s.n);

--result should match previous result
select count(*) from s where s.u in (select l.u from l where l.n = s.n);

reset enable_correlated_any_transform;

explain (costs off) select count(*) from s where s.n in (select l.n from l where l.u != s.u);

select count(*) from s where s.n in (select l.n from l where l.u != s.u);

set enable_correlated_any_transform = off;

explain (costs off) select count(*) from s where s.n in (select l.n from l where l.u != s.u);

--result should match previous result
select count(*) from s where s.n in (select l.n from l where l.u != s.u);

reset enable_correlated_any_transform;

explain (costs off) select count(*) from s where s.n in (select l.n from l where l.u > s.u);

select count(*) from s where s.n in (select l.n from l where l.u > s.u);

set enable_correlated_any_transform = off;

explain (costs off) select count(*) from s where s.n in (select l.n from l where l.u > s.u);

--result should match previous result
select count(*) from s where s.n in (select l.n from l where l.u > s.u);

reset enable_correlated_any_transform;

--test corrrelated NOT IN subquery
explain (costs off) select count(*) from s where s.u not in (select l.u from l where l.n = s.n);

select count(*) from s where s.u not in (select l.u from l where l.n = s.n);

set enable_correlated_any_transform = off;

explain (costs off) select count(*) from s where s.u not in (select l.u from l where l.n = s.n);

--result should match previous result
select count(*) from s where s.u not in (select l.u from l where l.n = s.n);

reset enable_correlated_any_transform;

explain (costs off) select count(*) from s where s.n not in (select l.n from l where l.u != s.u);

select count(*) from s where s.n not in (select l.n from l where l.u != s.u);

set enable_correlated_any_transform = off;

explain (costs off) select count(*) from s where s.n not in (select l.n from l where l.u != s.u);

--result should match previous result
select count(*) from s where s.n not in (select l.n from l where l.u != s.u);

reset enable_correlated_any_transform;

explain (costs off) select count(*) from s where s.n not in (select l.n from l where l.u > s.u);

select count(*) from s where s.n not in (select l.n from l where l.u > s.u);

set enable_correlated_any_transform = off;

explain (costs off) select count(*) from s where s.n not in (select l.n from l where l.u > s.u);

--result should match previous result
select count(*) from s where s.n not in (select l.n from l where l.u > s.u);

reset enable_correlated_any_transform;

--correlated empty subquery
explain (costs off) select count(*) from l where n not in (select n from empty where u != l.u);

select count(*) from l where n not in (select n from empty where u != l.u);

set enable_correlated_any_transform = off;

explain (costs off) select count(*) from l where n not in (select n from empty where u != l.u);

select count(*) from l where n not in (select n from empty where u != l.u);

reset enable_correlated_any_transform;

--nested correlated in, correlated var is two levels deep
explain (costs off) select * from s where n in (select n from s1 where n in (select n from l where u != s.u));

select * from s where n in (select n from s1 where n in (select n from l where u != s.u));

set enable_correlated_any_transform = off;

explain (costs off) select * from s where n in (select n from s1 where n in (select n from l where u != s.u));

select * from s where n in (select n from s1 where n in (select n from l where u != s.u));

reset enable_correlated_any_transform;

--nested correlated in, correlated var is one level deep
explain (costs off) select * from s where n in (select n from s1 where n in (select n from l) and u != s.u);

select * from s where n in (select n from s1 where n in (select n from l) and u != s.u);

set enable_correlated_any_transform = off;

explain (costs off) select * from s where n in (select n from s1 where n in (select n from l) and u != s.u);

select * from s where n in (select n from s1 where n in (select n from l) and u != s.u);

reset enable_correlated_any_transform;

--nested correlated in, correlated var is both one level and two levels deep
explain (costs off) select * from s where n in (select n from s1 where n in (select n from l where u != s.u) and u != s.u);

select * from s where n in (select n from s1 where n in (select n from l where u != s.u) and u != s.u);

set enable_correlated_any_transform = off;

explain (costs off) select * from s where n in (select n from s1 where n in (select n from l where u != s.u) and u != s.u);

select * from s where n in (select n from s1 where n in (select n from l where u != s.u) and u != s.u);

reset enable_correlated_any_transform;

--nested correlated not in, correlated var is both one level and two levels deep
explain (costs off) select * from s where n not in (select n from s1 where n not in (select n from l where u != s.u) and u != s.u);

select * from s where n not in (select n from s1 where n not in (select n from l where u != s.u) and u != s.u);

set enable_correlated_any_transform = off;

explain (costs off) select * from s where n not in (select n from s1 where n not in (select n from l where u != s.u) and u != s.u);

select * from s where n not in (select n from s1 where n not in (select n from l where u != s.u) and u != s.u);

reset enable_correlated_any_transform;

--nested correlated in and not in, correlated var is both one level and two levels deep
explain (costs off) select * from s where n in (select n from s1 where n not in (select n from l where u != s.u) and u != s.u);

select * from s where n in (select n from s1 where n not in (select n from l where u != s.u) and u != s.u);

set enable_correlated_any_transform = off;

explain (costs off) select * from s where n in (select n from s1 where n not in (select n from l where u != s.u) and u != s.u);

select * from s where n in (select n from s1 where n not in (select n from l where u != s.u) and u != s.u);

reset enable_correlated_any_transform;

--recursive cte & correlated not in
EXPLAIN (COSTS FALSE) WITH RECURSIVE managertree AS (
  SELECT id, name, manager_id
  FROM employees
  WHERE id = 2
  UNION ALL
  SELECT e.id, e.name, e.manager_id
  FROM employees e
  INNER JOIN managertree mtree ON mtree.id = e.manager_id
)
SELECT *
FROM managertree mt WHERE mt.manager_id NOT IN (SELECT id FROM managertree where managertree.name != mt.name);

WITH RECURSIVE managertree AS (
  SELECT id, name, manager_id
  FROM employees
  WHERE id = 2
  UNION ALL
  SELECT e.id, e.name, e.manager_id
  FROM employees e
  INNER JOIN managertree mtree ON mtree.id = e.manager_id
)
SELECT *
FROM managertree mt WHERE mt.manager_id NOT IN (SELECT id FROM managertree where managertree.name != mt.name);

set enable_correlated_any_transform = off;

EXPLAIN (COSTS FALSE) WITH RECURSIVE managertree AS (
  SELECT id, name, manager_id
  FROM employees
  WHERE id = 2
  UNION ALL
  SELECT e.id, e.name, e.manager_id
  FROM employees e
  INNER JOIN managertree mtree ON mtree.id = e.manager_id
)
SELECT *
FROM managertree mt WHERE mt.manager_id NOT IN (SELECT id FROM managertree where managertree.name != mt.name);

--result should match previous result
WITH RECURSIVE managertree AS (
  SELECT id, name, manager_id
  FROM employees
  WHERE id = 2
  UNION ALL
  SELECT e.id, e.name, e.manager_id
  FROM employees e
  INNER JOIN managertree mtree ON mtree.id = e.manager_id
)
SELECT *
FROM managertree mt WHERE mt.manager_id NOT IN (SELECT id FROM managertree where managertree.name != mt.name);

reset enable_correlated_any_transform;

--correlated var in select list, disallow transform
explain (costs off) select count(*) from s where (s.n, s.u) in (select l.n, s.nn from l where l.u != s.u);

select count(*) from s where (s.n, s.u) in (select l.n, s.nn from l where l.u != s.u);

--correlated var in select list and buried in an expression, disallow transform
explain (costs off) select count(*) from s where s.n in (select l.n * s.nn from l where l.u != s.u);

select count(*) from s where s.n in (select l.n * s.nn from l where l.u != s.u);

--correlated var in join ... on (...), disallow transform
explain (costs off)  select count(*) from s s1 where s1.n in (select l.n from l left join s s2 on s2.nn*s1.nn = l.nn where s1.u != l.u);

select count(*) from s s1 where s1.n in (select l.n from l left join s s2 on s2.nn*s1.nn = l.nn where s1.u != l.u);

--correlated var in order by, disallow transform
explain (costs off) select count(*) from s where s.n in (select l.n from l where l.u != s.u order by s.nn);

select count(*) from s where s.n in (select l.n from l where l.u != s.u order by s.nn);

explain (costs off) select count(*) from s where s.n in (select l.n from l where l.u != s.u order by s.nn - l.nn);

select count(*) from s where s.n in (select l.n from l where l.u != s.u order by s.nn - l.nn);

--correlated var in group by, disallow transfer since correlated var must also be in the select list
explain (costs off) select count(*) from s where (s.n, s.nn) in (select l.n, s.nn from l where l.u != s.u group by l.n, s.nn);

select count(*) from s where (s.n, s.nn) in (select l.n, s.nn from l where l.u != s.u group by l.n, s.nn);

explain (costs off) select count(*) from s where (s.n, s.nn) in (select l.n, s.nn from l where l.u != s.u group by l.n, s.nn - l.nn);

select count(*) from s where (s.n, s.nn) in (select l.n, s.nn from l where l.u != s.u group by l.n, s.nn - l.nn);

--correlated var in having clause, disallow transfer sinnce correlated var must also be in the select list
explain (costs off) select count(*) from s where (s.n, s.nn) in (select l.n, s.nn from l where l.u != s.u group by l.n having s.nn > 3);

select count(*) from s where (s.n, s.nn) in (select l.n, s.nn from l where l.u != s.u group by l.n having s.nn > 3);

-- clean up
drop table s;
drop table s1;
drop table l;
drop table empty;
drop table employees;
