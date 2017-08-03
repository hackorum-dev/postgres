-- Serializable isolation would disable parallel query, so explicitly use an
-- arbitrary other level.
begin isolation level repeatable read;

-- encourage use of parallel plans
set parallel_setup_cost=0;
set parallel_tuple_cost=0;
set min_parallel_table_scan_size=0;
set max_parallel_workers_per_gather=4;

-- test parallel bitmap heap scan.
set enable_seqscan to off;
set enable_indexscan to off;
set enable_hashjoin to off;
set enable_mergejoin to off;
set enable_material to off;
-- test prefetching, if the platform allows it
DO $$
BEGIN
 SET effective_io_concurrency = 50;
EXCEPTION WHEN invalid_parameter_value THEN
END $$;
set work_mem='64kB';  --set small work mem to force lossy pages
explain (costs off)
	select count(*) from tenk1, tenk2 where tenk1.hundred > 1 and tenk2.thousand=0;
select count(*) from tenk1, tenk2 where tenk1.hundred > 1 and tenk2.thousand=0;

rollback;
