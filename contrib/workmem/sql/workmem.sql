load 'workmem';

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

--====
-- Test suite 1: default workmem.query_work_mem (= 100 MB)
--====

----
-- Some tests from src/test/regress/sql/workmem.sql that don't require
-- test_setup.sql, etc., to be run first.
----

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

--====
-- Test suite 2: set workmem.query_work_mem to 4 MB
--====
set workmem.query_work_mem = 4096;

----
-- Some tests from src/test/regress/sql/workmem.sql that don't require
-- test_setup.sql, etc., to be run first.
----

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

reset workmem.query_work_mem;

--====
-- Test suite 3: set workmem.query_work_mem to 80 KB
--====
set workmem.query_work_mem = 80;

----
-- Some tests from src/test/regress/sql/workmem.sql that don't require
-- test_setup.sql, etc., to be run first.
----

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

reset workmem.query_work_mem;
