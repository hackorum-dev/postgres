--
-- Tests for evaluating scan qualifications in tuple batches
--

CREATE TABLE scan_batch_test (id int, a int, t text)
  WITH (autovacuum_enabled = false);

INSERT INTO scan_batch_test
SELECT g,
	CASE WHEN g IN (1, 2) THEN NULL ELSE g END,
	CASE WHEN g IN (3, 100) THEN NULL ELSE 'value-' || g END
FROM generate_series(1, 200) AS g;

-- Multiple batch qualifications, both argument orders, and NULL attributes.
SELECT count(*) FROM scan_batch_test
WHERE 0 < a AND a < 201 AND t <> 'missing';

-- An unsupported first qualification must leave the whole expression scalar.
SELECT count(*) FROM scan_batch_test
WHERE length(t) > 0 AND a > 50;

-- A supported prefix followed by a scalar remainder.
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT count(*) FROM scan_batch_test
WHERE a > 50 AND length(t) = 8;

-- PARAM_EXTERN, including a pass-by-reference value and NULL parameters.
SET plan_cache_mode = force_generic_plan;
PREPARE scan_batch_ext(int, text) AS
SELECT count(*) FROM scan_batch_test
WHERE $1 < a AND t <> $2;

EXECUTE scan_batch_ext(50, 'missing');
EXECUTE scan_batch_ext(NULL, 'missing');
EXECUTE scan_batch_ext(50, NULL);

DEALLOCATE scan_batch_ext;
RESET plan_cache_mode;

-- PARAM_EXEC values and rescans of the inner sequential scan.
SELECT p, q,
	(SELECT count(*) FROM scan_batch_test AS b
	 WHERE o.p < b.a AND b.t <> o.q) AS matches
FROM (VALUES
	(1, 0, 'missing'::text),
	(2, 100, 'value-150'),
	(3, NULL, 'missing'),
	(4, 0, NULL)
) AS o(ord, p, q)
ORDER BY ord;

-- A stateful subplan in the scalar remainder must follow parameter changes.
CREATE TABLE scan_batch_lookup (k int, v int);
INSERT INTO scan_batch_lookup
SELECT -1, g FROM generate_series(1, 200) AS g
UNION ALL
SELECT -2, g FROM generate_series(1, 100) AS g;

EXPLAIN (COSTS OFF)
SELECT o.p,
	(SELECT count(*) FROM scan_batch_test AS b
	 WHERE b.id > o.p
	   AND b.id NOT IN
		   (SELECT l.v FROM scan_batch_lookup AS l WHERE l.k = o.p)) AS matches
FROM (VALUES (1, -1), (2, -2)) AS o(ord, p)
ORDER BY o.ord;

SELECT o.p,
	(SELECT count(*) FROM scan_batch_test AS b
	 WHERE b.id > o.p
	   AND b.id NOT IN
		   (SELECT l.v FROM scan_batch_lookup AS l WHERE l.k = o.p)) AS matches
FROM (VALUES (1, -1), (2, -2)) AS o(ord, p)
ORDER BY o.ord;

DROP TABLE scan_batch_lookup;

-- Cross batch windows and change direction while projecting values.
BEGIN;
DECLARE scan_batch_cursor SCROLL CURSOR FOR
SELECT id + 1 AS projected, upper(t) AS projected_text
FROM scan_batch_test
WHERE id > 0 AND length(COALESCE(t, '')) >= 0;

MOVE FORWARD 130 FROM scan_batch_cursor;
FETCH BACKWARD 3 FROM scan_batch_cursor;
FETCH FORWARD 3 FROM scan_batch_cursor;
COMMIT;

-- System columns must follow the current tuple in a batch.
SELECT count(*)
FROM scan_batch_test AS s
WHERE s.id > 0
	AND NOT EXISTS (SELECT 1 FROM scan_batch_test AS tid_check
					WHERE tid_check.ctid = s.ctid AND tid_check.id = s.id);

-- The batch scan must return the projection's empty result slot.
SELECT id + 1 AS projected, upper(t) AS projected_text
FROM scan_batch_test
WHERE id > 200 AND length(COALESCE(t, '')) >= 0;

-- Calls made while preparing a batch must honor track_functions.
CREATE FUNCTION scan_batch_lt(int, int) RETURNS boolean
LANGUAGE plpgsql IMMUTABLE STRICT LEAKPROOF
AS $$BEGIN RETURN $1 < $2; END$$;

CREATE OPERATOR #<# (
	FUNCTION = scan_batch_lt,
	LEFTARG = int,
	RIGHTARG = int
);

SELECT 'scan_batch_lt(integer,integer)'::regprocedure::oid AS batch_func_oid \gset
SET track_functions = 'all';

SELECT count(*) FROM scan_batch_test WHERE a #<# 201;
SELECT pg_stat_force_next_flush() \gset
SELECT calls FROM pg_stat_user_functions WHERE funcid = :batch_func_oid;

-- Updating returned rows must not cause a batch to be evaluated repeatedly.
SELECT pg_stat_reset_single_function_counters(:batch_func_oid) AS reset \gset
UPDATE scan_batch_test SET t = t WHERE a #<# 201;
SELECT pg_stat_force_next_flush() \gset
SELECT calls FROM pg_stat_user_functions WHERE funcid = :batch_func_oid;

-- Relations that can receive in-place updates must use scalar evaluation.
SELECT pg_stat_reset_single_function_counters(:batch_func_oid) AS reset \gset
SELECT count(*) FROM
	(SELECT FROM pg_class
	 WHERE relpages #<# 2147483647
	 LIMIT 1) AS core_catalog;
SELECT pg_stat_force_next_flush() \gset
SELECT calls FROM pg_stat_user_functions WHERE funcid = :batch_func_oid;

ALTER TABLE scan_batch_test SET (user_catalog_table = true);
SELECT pg_stat_reset_single_function_counters(:batch_func_oid) AS reset \gset
SELECT count(*) FROM
	(SELECT FROM scan_batch_test
	 WHERE id #<# 201
	 LIMIT 1) AS user_catalog;
SELECT pg_stat_force_next_flush() \gset
SELECT calls FROM pg_stat_user_functions WHERE funcid = :batch_func_oid;

RESET track_functions;
DROP OPERATOR #<# (int, int);
DROP FUNCTION scan_batch_lt(int, int);
DROP TABLE scan_batch_test;
