SET enable_hashjoin = OFF;
SET enable_nestloop = OFF;
SET enable_mergejoin = ON;
SET enable_mergejoin_semijoin_filter = ON;
SET force_mergejoin_semijoin_filter = ON;

CREATE TABLE t1 (
  i integer,
  j integer
);

CREATE TABLE t2 (
  i integer,
  k integer
);

CREATE TABLE t3 (
  i integer,
  m integer
);

insert into t1 (i, j)
  select
    generate_series(1,100000) as i,
    generate_series(1,100000) as j;

insert into t2 (i, k)
  select
    generate_series(1,100000) as i,
    generate_series(1,100000) as k;

insert into t3 (i, m)
  select
    generate_series(1,100000) as i,
    generate_series(1,100000) as m;

-- Semijoin filter is not used when force_mergejoin_semijoin_filter is OFF.
SET force_mergejoin_semijoin_filter = OFF;
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(*) FROM t1 JOIN t2 ON t1.i = t2.i;
SET force_mergejoin_semijoin_filter = ON;

-- One level of inner mergejoin: push semi-join filter to outer scan.
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(*) FROM t1 JOIN t2 ON t1.i = t2.i;
SELECT COUNT(*) FROM t1 JOIN t2 ON t1.i = t2.i;

-- Push semijoin filter through SORT node.
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(*) FROM (SELECT DISTINCT t1.i FROM t1 ORDER BY t1.i) x JOIN t2 ON x.i = t2.i;
SELECT COUNT(*) FROM (SELECT DISTINCT t1.i FROM t1 ORDER BY t1.i) x JOIN t2 ON x.i = t2.i;

-- Push semijoin filter through LIMIT node: not supported.
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(*) FROM (SELECT DISTINCT t1.i FROM t1 ORDER BY t1.i LIMIT 1) x JOIN t2 ON x.i = t2.i;

-- Push SJF through MAX: not supported.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT COUNT(*) FROM (SELECT MAX(t1.i) AS i, t1.j FROM t1 GROUP BY t1.j) x JOIN t2 ON x.i = t2.i;

-- SJF with UNION: not supported.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT COUNT(*) FROM (SELECT t1.i FROM t1 UNION ALL SELECT t2.i FROM t2) x JOIN t3 ON x.i = t3.i;

-- Join clause is an expression: not supported.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT COUNT(*) FROM t1 JOIN t2 ON 1 + t1.i = t2.i;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT COUNT(*) FROM t1 JOIN t2 ON t1.i = 2 * t2.i;

-- Two levels of MergeJoin
EXPLAIN (VERBOSE, COSTS OFF)
SELECT COUNT(*) FROM t1 JOIN t2 ON t1.i = t2.i JOIN t3 ON t2.i = t3.i;
SELECT COUNT(*) FROM t1 JOIN t2 ON t1.i = t2.i JOIN t3 ON t2.i = t3.i;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT COUNT(*) FROM t1 JOIN (t2 JOIN t3 ON t2.i = t3.i) ON t1.i = t2.i;
SELECT COUNT(*) FROM t1 JOIN (t2 JOIN t3 ON t2.i = t3.i) ON t1.i = t2.i;

EXPLAIN (VERBOSE, COSTS OFF) 
SELECT COUNT(*) FROM (SELECT * FROM t1 JOIN t2 ON t1.i = t2.i) x JOIN t2 y ON x.k = y.k;
SELECT COUNT(*) FROM (SELECT * FROM t1 JOIN t2 ON t1.i = t2.i) x JOIN t2 y ON x.k = y.k;

-- Test Parallel Query
SET max_parallel_workers_per_gather = 8;
SET max_parallel_workers = 8;
SET min_parallel_table_scan_size = 0;
ALTER TABLE t1 SET (parallel_workers = 8);
ALTER TABLE t2 SET (parallel_workers = 8);
ALTER TABLE t3 SET (parallel_workers = 8);

-- one level of merge join
-- inner join: push bloom filter to outer scan
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(*) FROM t1 JOIN t2 ON t1.i = t2.i;
SELECT COUNT(*) FROM t1 JOIN t2 ON t1.i = t2.i;

-- Two levels of  merge join
EXPLAIN (VERBOSE, COSTS OFF) 
SELECT COUNT(*) FROM (SELECT * FROM t1 JOIN t2 ON t1.i = t2.i) x JOIN t2 y ON x.k = y.k;
SELECT COUNT(*) FROM (SELECT * FROM t1 JOIN t2 ON t1.i = t2.i) x JOIN t2 y ON x.k = y.k;


RESET max_parallel_workers_per_gather;
RESET max_parallel_workers;
RESET min_parallel_table_scan_size;

DROP TABLE t1;
DROP TABLE t2;
DROP TABLE t3;
RESET enable_mergejoin;
RESET enable_memoize;
RESET enable_mergejoin;
RESET enable_mergejoin_semijoin_filter;
RESET force_mergejoin_semijoin_filter;
