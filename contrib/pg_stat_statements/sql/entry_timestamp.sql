--
-- statement timestamps
--

-- planning time is needed during tests
SET pg_stat_statements.track_planning = TRUE;

SELECT 1 AS "STMTTS1";
SELECT now() AS ref_ts \gset
SELECT 1,2 AS "STMTTS2";
SELECT stats_since >= :'ref_ts', count(*) FROM pg_stat_statements
WHERE query LIKE '%STMTTS%'
GROUP BY stats_since >= :'ref_ts'
ORDER BY stats_since >= :'ref_ts';

SELECT now() AS ref_ts \gset
SELECT
  count(*) as total,
  count(*) FILTER (
    WHERE min_plan_time + max_plan_time = 0
  ) as minmax_plan_zero,
  count(*) FILTER (
    WHERE min_exec_time + max_exec_time = 0
  ) as minmax_exec_zero,
  count(*) FILTER (
    WHERE minmax_stats_since >= :'ref_ts'
  ) as minmax_stats_since_after_ref,
  count(*) FILTER (
    WHERE stats_since >= :'ref_ts'
  ) as stats_since_after_ref
FROM pg_stat_statements
WHERE query LIKE '%STMTTS%';

-- Perform single min/max reset
SELECT pg_stat_statements_reset(0, 0, queryid, true) AS minmax_reset_ts
FROM pg_stat_statements
WHERE query LIKE '%STMTTS1%' \gset

-- check
SELECT
  count(*) as total,
  count(*) FILTER (
    WHERE min_plan_time + max_plan_time = 0
  ) as minmax_plan_zero,
  count(*) FILTER (
    WHERE min_exec_time + max_exec_time = 0
  ) as minmax_exec_zero,
  count(*) FILTER (
    WHERE minmax_stats_since >= :'ref_ts'
  ) as minmax_stats_since_after_ref,
  count(*) FILTER (
    WHERE stats_since >= :'ref_ts'
  ) as stats_since_after_ref
FROM pg_stat_statements
WHERE query LIKE '%STMTTS%';

-- check minmax reset timestamps
SELECT
query, minmax_stats_since = :'minmax_reset_ts' AS reset_ts_match
FROM pg_stat_statements
WHERE query LIKE '%STMTTS%'
ORDER BY query COLLATE "C";

-- check that minmax reset does not set stats_reset
SELECT
stats_reset = :'minmax_reset_ts' AS stats_reset_ts_match
FROM pg_stat_statements_info;

-- Perform common min/max reset
SELECT pg_stat_statements_reset(0, 0, 0, true) AS minmax_reset_ts \gset

-- check again
SELECT
  count(*) as total,
  count(*) FILTER (
    WHERE min_plan_time + max_plan_time = 0
  ) as minmax_plan_zero,
  count(*) FILTER (
    WHERE min_exec_time + max_exec_time = 0
  ) as minmax_exec_zero,
  count(*) FILTER (
    WHERE minmax_stats_since >= :'ref_ts'
  ) as minmax_ts_after_ref,
  count(*) FILTER (
    WHERE minmax_stats_since = :'minmax_reset_ts'
  ) as minmax_ts_match,
  count(*) FILTER (
    WHERE stats_since >= :'ref_ts'
  ) as stats_since_after_ref
FROM pg_stat_statements
WHERE query LIKE '%STMTTS%';

-- Execute first query once more to check stats update
SELECT 1 AS "STMTTS1";

-- check
-- we don't check planing times here to be independent of
-- plan caching approach
SELECT
  count(*) as total,
  count(*) FILTER (
    WHERE min_exec_time + max_exec_time = 0
  ) as minmax_exec_zero,
  count(*) FILTER (
    WHERE minmax_stats_since >= :'ref_ts'
  ) as minmax_ts_after_ref,
  count(*) FILTER (
    WHERE stats_since >= :'ref_ts'
  ) as stats_since_after_ref
FROM pg_stat_statements
WHERE query LIKE '%STMTTS%';

--
-- stats_last_updated timestamp tests
--

SELECT 1 AS "STATS_UPD1";
SELECT now() AS ref_ts_upd1 \gset
SELECT pg_sleep(0.1);
SELECT 2 AS "STATS_UPD2";
SELECT now() AS ref_ts_upd2 \gset

-- verify stats_last_updated is set and updated
SELECT
    query,
    stats_last_updated IS NOT NULL as has_ts,
    stats_last_updated >= :'ref_ts_upd1' as after_ref1,
    stats_since <= stats_last_updated as after_stats_since
FROM pg_stat_statements
WHERE query LIKE '%STATS_UPD%'
ORDER BY query COLLATE "C";

-- execute again and verify update
SELECT pg_sleep(0.1);
SELECT 1 AS "STATS_UPD1";
SELECT now() AS ref_ts_upd3 \gset

SELECT
    query,
    stats_last_updated >= :'ref_ts_upd3' as updated
FROM pg_stat_statements
WHERE query LIKE '%STATS_UPD1%';

-- test filtering (monitoring use case)
SELECT count(*) as filtered_count
FROM pg_stat_statements
WHERE stats_last_updated >= :'ref_ts_upd2'
  AND query LIKE '%STATS_UPD%';

-- minmax reset should not affect stats_last_updated
SELECT pg_stat_statements_reset(0, 0, queryid, true)
FROM pg_stat_statements
WHERE query LIKE '%STATS_UPD1%' \gset

SELECT
    query,
    stats_last_updated >= :'ref_ts_upd3' as ts_preserved
FROM pg_stat_statements
WHERE query LIKE '%STATS_UPD1%';

-- Cleanup
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
