-- B-Tree Merge Scan Access Method Test
--
-- B-Tree Merge Scan is an access method that allows lazily producing
-- output sorted by a non-leading column when the prefix has few distinct values.
--
--
-- Let S be an infinite set of lattic points (x,y).
-- Let S(x=1,y>=b) be the sequence of points 
--     SELECT * FROM S WHERE x = a and y >= b ORDER BY b;
--     i.e. (a, b), (a, b+1), (a, b+2), ...
-- Similarly, S(x IN X, y=b) being the sequence of points
--     SELECT * FROM S WHERE x IN X and y = b ORDER BY x;
--     i.e. (x[1], b), ..., (x[n], b), (x[1], b+1), ...
-- The output of S(x IN X, y >= b) can be computed as a
--
-- Proposition (uncomputable): 
-- S(x, IN X, y >= b) is the K-way merge of the sequences 
-- {S(x=x[i], y >= b), x[i] in X}
-- 
--
--
-- Proposition (computable): Bounded suffix
--
-- S(x, IN X, b1 <= y <= b2) as bounded
-- can be computed with (SELECT count(distinct x) + count(1) FROM bounded)
-- tuple accesses.
-- (Constructive) Proof:
-- The result of 
--    SELECT * FROM X 
--    JOIN S on x = x[i] WHERE y BETWEEN b1 AND b2;
-- is the same as 
--    SELECT * FROM X,
--    LATERAL (
--        (SELECT * FROM S 
--           WHERE x = x[i] AND y BETWEEN b1 AND b2
--        ) AS subscan[i]
--    ) as merged
-- 
-- Each of subscan[i] is covered by a single range in the index and can
-- and require at most 
--  (count(1) FROM subscan[i]) + 1    -- subscan tuple access count
-- tupples to be accessed.
-- The merged result can be computed using a K-way merge sort 
-- whose number of rows is
--   sum(count(1) FROM subscan[i])    -- query output rows
-- Q.E.D.
--
--
-- Proposition (computable): Limitted query
-- The query
--   S(x, IN X, y >= b) LIMIT N as limited
-- Can be computed with at most 
--   N + count(distinct X) - 1
-- tuple accesses.
--
-- (Constructive) Proof:
-- If an upper `u` bound for `MAX(y IN S(x, IN X, y >= b) LIMIT N)` is known,
-- then the query can be rewritten as
--   S(x, IN X, b <= y <= u) LIMIT N
-- The K-way can produce the next element as soon as it has fetched
-- the next element for each subquery
-- 1 row can be produced after count(distinct X) fetches,
-- After that it can produce one new row for each fetch.
-- Thus, the total number of fetches is at most
--   N + count(distinct X) - 1
-- Q.E.D.


-- Generate a table with lattice points 
-- Could be infinite
CREATE TABLE btree_merge_test AS (
    SELECT x, y FROM 
        generate_series(1, 50) AS x, 
        generate_series(1, 50) AS y
    ORDER BY random()
);
CREATE INDEX btree_merge_test_idx ON btree_merge_test USING btree (x, y);

ANALYSE btree_merge_test;

SET enable_seqscan = OFF;
SET enable_bitmapscan = OFF;
SHOW track_counts;  -- should be 'on'

-- Verify merge scan is used: no Sort node when ORDER BY suffix only
-- K = 8 prefixes, LIMIT 3 -> reads at most 3 + 8 - 1 = 10 tuples
EXPLAIN (COSTS OFF)
SELECT x, y
FROM btree_merge_test
WHERE x IN (1,2,5,8,13,21,34,55) AND y >= 19
ORDER BY y
LIMIT 3;

-- Verify the query produces correct results (sorted by y)
SELECT x, y
FROM btree_merge_test
WHERE x IN (1,2,5,8,13,21,34,55) AND y >= 19
ORDER BY y
LIMIT 3;


SELECT pg_stat_force_next_flush();


SELECT idx_scan, idx_tup_read, idx_tup_fetch 
FROM pg_stat_user_indexes 
WHERE indexrelname = 'btree_merge_test_idx';

DROP TABLE btree_merge_test;

-- ============================================
-- Multi-column prefix tests
-- ============================================

-- Create a 3-column table for multi-prefix testing
CREATE TABLE btree_merge_multi AS (
    SELECT a, b, c FROM 
        generate_series(1, 10) AS a, 
        generate_series(1, 10) AS b,
        generate_series(1, 20) AS c
    ORDER BY random()
);
CREATE INDEX btree_merge_multi_idx ON btree_merge_multi USING btree (a, b, c);
ANALYSE btree_merge_multi;

-- Test 1: a = const AND b IN B -> 3 cursors (just the IN list)
-- Merge scan triggered, no Sort node when ORDER BY suffix only
EXPLAIN (COSTS OFF)
SELECT a, b, c
FROM btree_merge_multi
WHERE a = 1 AND b IN (1, 2, 3) AND c >= 5
ORDER BY c
LIMIT 3;

SELECT a, b, c
FROM btree_merge_multi
WHERE a = 1 AND b IN (1, 2, 3) AND c >= 5
ORDER BY c
LIMIT 3;

-- Test 2: a IN A AND b IN B -> len(A) * len(B) cursors (Cartesian product)
-- With a IN (1,2), b IN (1,2,3), ORDER BY c LIMIT 4
-- Should use 6 cursors (2*3), no Sort node needed
EXPLAIN (COSTS OFF)
SELECT a, b, c
FROM btree_merge_multi
WHERE a IN (1, 2) AND b IN (1, 2, 3) AND c >= 10
ORDER BY c
LIMIT 4;

SELECT a, b, c
FROM btree_merge_multi
WHERE a IN (1, 2) AND b IN (1, 2, 3) AND c >= 10
ORDER BY c
LIMIT 4;

-- Test 3: a IN A AND b = const -> len(A) cursors
-- With a IN (1,2,3,4), b=5, ORDER BY c LIMIT 2
-- Should use 4 cursors, no Sort node needed
EXPLAIN (COSTS OFF)
SELECT a, b, c
FROM btree_merge_multi
WHERE a IN (1, 2, 3, 4) AND b = 5 AND c >= 8
ORDER BY c
LIMIT 2;

SELECT a, b, c
FROM btree_merge_multi
WHERE a IN (1, 2, 3, 4) AND b = 5 AND c >= 8
ORDER BY c
LIMIT 2;

-- Test 4: Backward scan direction (ORDER BY DESC)
-- With a IN (1,2,3), b IN (1,2), ORDER BY c DESC LIMIT 3
-- Should use 6 cursors (3*2), no Sort node needed
EXPLAIN (COSTS OFF)
SELECT a, b, c
FROM btree_merge_multi
WHERE a IN (1, 2, 3) AND b IN (1, 2) AND c <= 15
ORDER BY c DESC
LIMIT 3;

SELECT a, b, c
FROM btree_merge_multi
WHERE a IN (1, 2, 3) AND b IN (1, 2) AND c <= 15
ORDER BY c DESC
LIMIT 3;

-- =================================================================
-- Multi-column suffix tests
-- Index is on (a, b, c), testing with prefix on 'a' only
-- =================================================================

-- Test 5: ORDER BY b (single column suffix)
-- With a IN (1,2,3), ORDER BY b LIMIT 6
-- Prefix: a, Suffix: b
EXPLAIN (COSTS OFF)
SELECT a, b, c
FROM btree_merge_multi
WHERE a IN (1, 2, 3) AND b >= 1
ORDER BY b
LIMIT 6;

SELECT a, b, c
FROM btree_merge_multi
WHERE a IN (1, 2, 3) AND b >= 1
ORDER BY b
LIMIT 6;

-- Test 6: ORDER BY b DESC (single column suffix, backward)
-- With a IN (1,2,3), ORDER BY b DESC LIMIT 6
EXPLAIN (COSTS OFF)
SELECT a, b, c
FROM btree_merge_multi
WHERE a IN (1, 2, 3) AND b <= 10
ORDER BY b DESC
LIMIT 6;

SELECT a, b, c
FROM btree_merge_multi
WHERE a IN (1, 2, 3) AND b <= 10
ORDER BY b DESC
LIMIT 6;

-- Test 7: ORDER BY b, c (multi-column suffix)
-- With a IN (1,2,3), ORDER BY b, c LIMIT 6
-- Prefix: a, Suffix: (b, c)
EXPLAIN (COSTS OFF)
SELECT a, b, c
FROM btree_merge_multi
WHERE a IN (1, 2, 3) AND b >= 1
ORDER BY b, c
LIMIT 6;

SELECT a, b, c
FROM btree_merge_multi
WHERE a IN (1, 2, 3) AND b >= 1
ORDER BY b, c
LIMIT 6;

-- Test 8: ORDER BY b DESC, c DESC (multi-column suffix, backward)
-- With a IN (1,2,3), ORDER BY b DESC, c DESC LIMIT 6
EXPLAIN (COSTS OFF)
SELECT a, b, c
FROM btree_merge_multi
WHERE a IN (1, 2, 3) AND b <= 10
ORDER BY b DESC, c DESC
LIMIT 6;

SELECT a, b, c
FROM btree_merge_multi
WHERE a IN (1, 2, 3) AND b <= 10
ORDER BY b DESC, c DESC
LIMIT 6;

DROP TABLE btree_merge_multi;