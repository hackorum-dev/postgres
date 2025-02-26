----
-- Tests that show "work_mem" output to EXPLAIN plans.
----

-- Note: Function derived from file explain.sql. We can't use that other
-- function, since we're run in parallel with explain.sql.
create or replace function workmem_filter(text) returns setof text
language plpgsql as
$$
declare
    ln text;
begin
    for ln in execute $1
    loop
        -- Mask out work_mem estimate, since it might be brittle
        ln := regexp_replace(ln, '\mwork_mem=\d+\M', 'work_mem=N', 'g');
        ln := regexp_replace(ln, '\mMemory Estimate: \d+\M', 'Memory Estimate: N', 'g');
        return next ln;
    end loop;
end;
$$;

-- Unique -> hash agg
set enable_hashagg = on;

select workmem_filter('
explain (costs off, work_mem on)
select *
from onek
where (unique1,ten) in (values (1,1), (20,0), (99,9), (17,99))
order by unique1;
');

select *
from onek
where (unique1,ten) in (values (1,1), (20,0), (99,9), (17,99))
order by unique1;

reset enable_hashagg;

-- Unique -> sort
set enable_hashagg = off;

select workmem_filter('
explain (costs off, work_mem on)
select *
from onek
where (unique1,ten) in (values (1,1), (20,0), (99,9), (17,99))
order by unique1;
');

select *
from onek
where (unique1,ten) in (values (1,1), (20,0), (99,9), (17,99))
order by unique1;

reset enable_hashagg;

-- Incremental Sort
select workmem_filter('
explain (costs off, work_mem on)
select * from (select * from tenk1 order by four) t order by four, ten
limit 1;
');

select * from (select * from tenk1 order by four) t order by four, ten
limit 1;

-- Hash Join
select workmem_filter('
explain (costs off, work_mem on)
select count(*) from (
select t1.unique1, t2.hundred
from onek t1, tenk1 t2
where exists (select 1 from tenk1 t3
              where t3.thousand = t1.unique1 and t3.tenthous = t2.hundred)
      and t1.unique1 < 1
) t;
');

select count(*) from (
select t1.unique1, t2.hundred
from onek t1, tenk1 t2
where exists (select 1 from tenk1 t3
              where t3.thousand = t1.unique1 and t3.tenthous = t2.hundred)
      and t1.unique1 < 1
) t;

-- Materialize
select workmem_filter('
explain (costs off, work_mem on)
select count(*) from (
select t1.f1
from int4_tbl t1, int4_tbl t2
  left join int4_tbl t3 on t3.f1 > 0
  left join int4_tbl t4 on t3.f1 > 1
where t4.f1 is null
) t;
');

select count(*) from (
select t1.f1
from int4_tbl t1, int4_tbl t2
  left join int4_tbl t3 on t3.f1 > 0
  left join int4_tbl t4 on t3.f1 > 1
where t4.f1 is null
) t;

-- Grouping Sets (Hash)
select workmem_filter('
explain (costs off, work_mem on)
select a, b, row_number() over (order by a, b nulls first)
from (values (1, 1), (2, 2)) as t (a, b) where a = b
group by grouping sets((a, b), (a));
');

select a, b, row_number() over (order by a, b nulls first)
from (values (1, 1), (2, 2)) as t (a, b) where a = b
group by grouping sets((a, b), (a));

-- Grouping Sets (Sort)
set enable_hashagg = off;

select workmem_filter('
explain (costs off, work_mem on)
select a, b, row_number() over (order by a, b nulls first)
from (values (1, 1, 1, 1), (2, 2, 2, 2)) as t (a, b, c, d) where a = b
group by grouping sets((a, b), (a), (b), (c), (d));
');

select a, b, row_number() over (order by a, b nulls first)
from (values (1, 1, 1, 1), (2, 2, 2, 2)) as t (a, b, c, d) where a = b
group by grouping sets((a, b), (a), (b), (c), (d));

reset enable_hashagg;

-- Agg (hash, parallel)
set parallel_setup_cost=0;
set parallel_tuple_cost=0;
set min_parallel_table_scan_size=0;
set max_parallel_workers_per_gather=4;

select workmem_filter('
explain (costs off, work_mem on)
select length(stringu1) from tenk1 group by length(stringu1);
');

select length(stringu1) from tenk1 group by length(stringu1);

reset parallel_setup_cost;
reset parallel_tuple_cost;
reset min_parallel_table_scan_size;
reset max_parallel_workers_per_gather;

-- Agg (simple) [no work_mem]
explain (costs off, work_mem on)
select MAX(length(stringu1)) from tenk1;

select MAX(length(stringu1)) from tenk1;

-- Function Scan
select workmem_filter('
explain (work_mem on, costs off)
select count(*) from (
select sum(n) over(partition by m)
from (SELECT n < 3 as m, n from generate_series(1,2000) a(n))
) t;
');

select count(*) from (
select sum(n) over(partition by m)
from (SELECT n < 3 as m, n from generate_series(1,2000) a(n))
) t;

-- Three Function Scans
select workmem_filter('
explain (work_mem on, costs off)
select count(*)
from rows from(generate_series(1, 5),
               generate_series(2, 10),
               generate_series(4, 15));
');

select count(*)
from rows from(generate_series(1, 5),
               generate_series(2, 10),
               generate_series(4, 15));

-- Table Function Scan
CREATE TABLE workmem_xmldata(data xml);

select workmem_filter('
EXPLAIN (COSTS OFF, work_mem on)
SELECT  xmltable.*
   FROM (SELECT data FROM workmem_xmldata) x,
        LATERAL XMLTABLE(''/ROWS/ROW''
                         PASSING data
                         COLUMNS id int PATH ''@id'',
                                  _id FOR ORDINALITY,
                                  country_name text PATH ''COUNTRY_NAME'' NOT NULL,
                                  country_id text PATH ''COUNTRY_ID'',
                                  region_id int PATH ''REGION_ID'',
                                  size float PATH ''SIZE'',
                                  unit text PATH ''SIZE/@unit'',
                                  premier_name text PATH ''PREMIER_NAME'' DEFAULT ''not specified'');
');

SELECT  xmltable.*
   FROM (SELECT data FROM workmem_xmldata) x,
        LATERAL XMLTABLE('/ROWS/ROW'
                         PASSING data
                         COLUMNS id int PATH '@id',
                                  _id FOR ORDINALITY,
                                  country_name text PATH 'COUNTRY_NAME' NOT NULL,
                                  country_id text PATH 'COUNTRY_ID',
                                  region_id int PATH 'REGION_ID',
                                  size float PATH 'SIZE',
                                  unit text PATH 'SIZE/@unit',
                                  premier_name text PATH 'PREMIER_NAME' DEFAULT 'not specified');

drop table workmem_xmldata;

-- SetOp [no work_mem]
explain (costs off, work_mem on)
select unique1 from tenk1 except select unique2 from tenk1 where unique2 != 10;

select unique1 from tenk1 except select unique2 from tenk1 where unique2 != 10;

-- HashSetOp
select workmem_filter('
explain (costs off, work_mem on)
select count(*) from
  ( select unique1 from tenk1 intersect select fivethous from tenk1 ) ss;
');

select count(*) from
  ( select unique1 from tenk1 intersect select fivethous from tenk1 ) ss;

-- RecursiveUnion and Memoize (also WorkTable Scan [no work_mem])
select workmem_filter('
explain (costs off, work_mem on)
select sum(o.four), sum(ss.a) from onek o
cross join lateral (with recursive x(a) as (
          select o.four as a union select a + 1 from x where a < 10)
    select * from x) ss where o.ten = 1;
');

select sum(o.four), sum(ss.a) from onek o
cross join lateral (with recursive x(a) as (
          select o.four as a union select a + 1 from x where a < 10)
    select * from x) ss where o.ten = 1;

-- CTE Scan
select workmem_filter('
explain (costs off, work_mem on)
WITH q1(x,y) AS (
    SELECT hundred, sum(ten) FROM tenk1 GROUP BY hundred
  )
SELECT count(*) FROM q1 WHERE y > (SELECT sum(y)/100 FROM q1 qsub);
');

WITH q1(x,y) AS (
    SELECT hundred, sum(ten) FROM tenk1 GROUP BY hundred
  )
SELECT count(*) FROM q1 WHERE y > (SELECT sum(y)/100 FROM q1 qsub);

-- WindowAgg
select workmem_filter('
explain (costs off, work_mem on)
select sum(n) over(partition by m)
from (SELECT n < 3 as m, n from generate_series(1,2000) a(n))
limit 5;
');

select sum(n) over(partition by m)
from (SELECT n < 3 as m, n from generate_series(1,2000) a(n))
limit 5;

-- Bitmap Heap Scan
select workmem_filter('
explain (costs off, work_mem on)
select count(*) from (
select * from tenk1 a join tenk1 b on
  (a.unique1 = 1 and b.unique1 = 2) or (a.unique2 = 3 and b.hundred = 4)
);
');

select count(*) from (
select * from tenk1 a join tenk1 b on
  (a.unique1 = 1 and b.unique1 = 2) or (a.unique2 = 3 and b.hundred = 4)
);

-- InitPlan with hash table ("IN SELECT")
select workmem_filter('
explain (costs off, work_mem on)
select ''foo''::text in (select ''bar''::name union all select ''bar''::name);
');

select 'foo'::text in (select 'bar'::name union all select 'bar'::name);

-- SubPlan with hash table
select workmem_filter('
explain (costs off, work_mem on)
select 1 = any (select (select 1) where 1 = any (select 1));
');

select 1 = any (select (select 1) where 1 = any (select 1));
