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
-- last_execution_start timestamp tests
--
-- Reset stats first to avoid queryId collisions: simple "SELECT const AS alias"
-- queries all share the same normalized structure as the STMTTS queries above,
-- so EXECSTART entries would otherwise land on the pre-existing STMTTS entry.
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
-- Capture a reference timestamp before running the tracked queries.
SELECT now() AS ref_ts_upd1 \gset
SELECT 1 AS "EXECSTART1";
-- last_execution_start should be set and >= ref_ts_upd1, because the
-- statement started after we captured the reference timestamp.
SELECT
    query,
    last_execution_start IS NOT NULL as has_ts,
    last_execution_start >= :'ref_ts_upd1' as after_ref1,
    stats_since <= last_execution_start as after_stats_since
FROM pg_stat_statements
WHERE query LIKE '%EXECSTART%'
ORDER BY query COLLATE "C";

-- Run EXECSTART1 again and verify that last_execution_start is updated.
SELECT now() AS ref_ts_upd2 \gset
SELECT 1 AS "EXECSTART1";
SELECT
    query,
    last_execution_start >= :'ref_ts_upd2' as updated
FROM pg_stat_statements
WHERE query LIKE '%EXECSTART1%';

-- test filtering (monitoring use case): find statements that started
-- executing since our last observation (ref_ts_upd2).
SELECT count(*) as filtered_count
FROM pg_stat_statements
WHERE last_execution_start >= :'ref_ts_upd2'
  AND query LIKE '%EXECSTART%';

-- minmax reset should not affect last_execution_start
SELECT pg_stat_statements_reset(0, 0, queryid, true)
FROM pg_stat_statements
WHERE query LIKE '%EXECSTART1%' \gset

SELECT
    query,
    last_execution_start >= :'ref_ts_upd2' as ts_preserved
FROM pg_stat_statements
WHERE query LIKE '%EXECSTART1%';

--
-- Deferred ExecutorEnd test (extended query protocol)
--
-- In the extended query protocol the previous query's ExecutorEnd is
-- deferred until the next Bind message, at which point
-- GetCurrentStatementStartTimestamp() already reflects the *new* query.
-- Verify that last_execution_start still records the *old* query's start.
--
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT now() AS ref_ts_ext \gset
-- Use \bind \g to force the extended query protocol.
SELECT pg_sleep(0.5) AS "DEFERRED_END" \bind \g
-- Capture a timestamp *after* the sleep finishes but *before* the next
-- extended-protocol statement replaces the unnamed portal.
SELECT now() AS ref_ts_ext2 \gset
SELECT 1 AS "TRIGGER_END" \bind \g
-- The pg_sleep query's last_execution_start should be close to ref_ts_ext
-- (before the sleep), NOT to ref_ts_ext2 (after the sleep).
SELECT
    query,
    last_execution_start >= :'ref_ts_ext' as after_start,
    last_execution_start < :'ref_ts_ext2' as before_next
FROM pg_stat_statements
WHERE query LIKE '%DEFERRED_END%'
ORDER BY query COLLATE "C";

-- Cleanup
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
