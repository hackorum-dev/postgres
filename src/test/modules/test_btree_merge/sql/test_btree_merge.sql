-- Unit tests for B-tree merge scan implementation
-- Tests the core merge scan algorithm directly, bypassing the planner

CREATE EXTENSION test_btree_merge;

-- ============================================================================
-- Setup: Create test tables with known data distributions
-- ============================================================================

-- Test table with integer prefix and suffix
CREATE TABLE merge_test_int (
    prefix_col int4,
    suffix_col int4
);

-- Insert data: 10 prefix values, 100 suffix values each = 1000 rows
INSERT INTO merge_test_int
SELECT p, s
FROM generate_series(1, 10) AS p,
     generate_series(1, 100) AS s;

CREATE INDEX merge_test_int_idx ON merge_test_int (prefix_col, suffix_col);
ANALYZE merge_test_int;

-- Test table with integer prefix and timestamp suffix
CREATE TABLE merge_test_ts (
    user_id int4,
    event_time timestamp
);

-- Insert data: 5 users, 100 events each
INSERT INTO merge_test_ts
SELECT u, '2026-01-01 00:00:00'::timestamp + (e || ' minutes')::interval
FROM generate_series(1, 5) AS u,
     generate_series(1, 100) AS e;

CREATE INDEX merge_test_ts_idx ON merge_test_ts (user_id, event_time);
ANALYZE merge_test_ts;


-- ============================================================================
-- Test 1: Basic integer merge scan
-- Query: WHERE prefix IN (1,2,3) AND suffix >= 50 LIMIT 5
-- K = 3 prefix values, LIMIT = 5
-- Expected tuples accessed: 5 + 3 - 1 = 7
-- ============================================================================

SELECT 'Test 1: Basic integer merge scan' AS test_name;

SELECT * FROM test_btree_merge_scan_int(
    'merge_test_int',
    'merge_test_int_idx',
    ARRAY[1, 2, 3],
    50,
    5
);


-- ============================================================================
-- Test 2: More prefix values
-- Query: WHERE prefix IN (1,2,3,4,5) AND suffix >= 80 LIMIT 3
-- K = 5 prefix values, LIMIT = 3
-- Expected tuples accessed: 3 + 5 - 1 = 7
-- ============================================================================

SELECT 'Test 2: More prefix values' AS test_name;

SELECT * FROM test_btree_merge_scan_int(
    'merge_test_int',
    'merge_test_int_idx',
    ARRAY[1, 2, 3, 4, 5],
    80,
    3
);


-- ============================================================================
-- Test 3: Single prefix value (degenerates to regular scan)
-- K = 1, LIMIT = 5
-- Expected tuples accessed: 5 + 1 - 1 = 5
-- ============================================================================

SELECT 'Test 3: Single prefix value' AS test_name;

SELECT * FROM test_btree_merge_scan_int(
    'merge_test_int',
    'merge_test_int_idx',
    ARRAY[1],
    50,
    5
);


-- ============================================================================
-- Test 4: Large LIMIT (more than matching rows)
-- K = 3, prefix values that have 51 rows each (suffix >= 50)
-- LIMIT = 200 but only 153 rows exist
-- ============================================================================

SELECT 'Test 4: Large LIMIT' AS test_name;

SELECT * FROM test_btree_merge_scan_int(
    'merge_test_int',
    'merge_test_int_idx',
    ARRAY[1, 2, 3],
    50,
    200
);


-- ============================================================================
-- Test 5: Non-contiguous prefix values
-- Query: WHERE prefix IN (2,5,8) AND suffix >= 50 LIMIT 5
-- Tests that merge scan works with gaps in prefix values
-- K = 3 prefix values (non-adjacent), LIMIT = 5
-- ============================================================================

SELECT 'Test 5: Non-contiguous prefix values' AS test_name;

SELECT * FROM test_btree_merge_scan_int(
    'merge_test_int',
    'merge_test_int_idx',
    ARRAY[2, 5, 8],
    50,
    5
);


-- ============================================================================
-- Test 6: Timestamp suffix column
-- Query: WHERE user_id IN (1,2,3) AND event_time >= '2026-01-01 01:00:00' LIMIT 5
-- K = 3, LIMIT = 5
-- Expected tuples accessed: 5 + 3 - 1 = 7
-- ============================================================================

SELECT 'Test 6: Timestamp suffix' AS test_name;

SELECT * FROM test_btree_merge_scan_ts(
    'merge_test_ts',
    'merge_test_ts_idx',
    ARRAY[1, 2, 3],
    '2026-01-01 01:00:00'::timestamp,
    5
);


-- ============================================================================
-- Test 7: All users with timestamp
-- K = 5, LIMIT = 10
-- Expected tuples accessed: 10 + 5 - 1 = 14
-- ============================================================================

SELECT 'Test 7: All users timestamp' AS test_name;

SELECT * FROM test_btree_merge_scan_ts(
    'merge_test_ts',
    'merge_test_ts_idx',
    ARRAY[1, 2, 3, 4, 5],
    '2026-01-01 00:30:00'::timestamp,
    10
);


-- ============================================================================
-- Test 8: Correctness verification
-- Verify merge scan returns rows in exact ORDER BY suffix_col, prefix_col order
-- Using WITH ORDINALITY to compare row positions
-- ============================================================================

SELECT 'Test 8: Correctness verification' AS test_name;

-- Compare merge scan vs regular query with row positions (should be empty)
WITH merge_result AS (
    SELECT row_number() OVER () AS rn, prefix_col, suffix_col
    FROM test_btree_merge_fetch_int(
        'merge_test_int',
        'merge_test_int_idx',
        ARRAY[1, 2, 3],
        90,
        10
    )
),
regular_result AS (
    SELECT row_number() OVER () AS rn, prefix_col, suffix_col
    FROM (
        SELECT prefix_col, suffix_col
        FROM merge_test_int
        WHERE prefix_col IN (1, 2, 3) AND suffix_col >= 90
        ORDER BY suffix_col, prefix_col
        LIMIT 10
    ) t
)
SELECT 'MISMATCH' AS status, m.rn, m.prefix_col, m.suffix_col,
       r.prefix_col AS expected_prefix, r.suffix_col AS expected_suffix
FROM merge_result m
FULL OUTER JOIN regular_result r ON m.rn = r.rn
WHERE m.prefix_col IS DISTINCT FROM r.prefix_col
   OR m.suffix_col IS DISTINCT FROM r.suffix_col;


-- ============================================================================
-- Cleanup
-- ============================================================================

DROP TABLE merge_test_int;
DROP TABLE merge_test_ts;
DROP EXTENSION test_btree_merge;
