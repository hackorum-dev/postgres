--
-- sample statements
--

-- top-level tracking - simple query protocol
SHOW pg_stat_statements.track;
SET pg_stat_statements.sample_rate = 0.0;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT 1 AS "int";
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";
SET pg_stat_statements.sample_rate = 1.0;
SELECT 1 AS "int";
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- top-level tracking - extended query protocol
SET pg_stat_statements.sample_rate = 0.0;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT 1 \parse stmt
\bind_named stmt \g
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";
SET pg_stat_statements.sample_rate = 1.0;
\bind_named stmt \g
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
DEALLOCATE stmt;

-- nested tracking - simple query protocol
SET pg_stat_statements.track = "all";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SET pg_stat_statements.sample_rate = 1;
EXPLAIN (COSTS OFF) SELECT 1;
EXPLAIN (COSTS OFF) SELECT 1;
SET pg_stat_statements.sample_rate = 0;
EXPLAIN (COSTS OFF) SELECT 1;
EXPLAIN (COSTS OFF) SELECT 1;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- nested tracking - extended query protocol
SET pg_stat_statements.track = "all";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SET pg_stat_statements.sample_rate = 1;
EXPLAIN (COSTS OFF) SELECT 1; \parse stmt
\bind_named stmt \g
\bind_named stmt \g
SET pg_stat_statements.sample_rate = 0;
\bind_named stmt \g
\bind_named stmt \g
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";
