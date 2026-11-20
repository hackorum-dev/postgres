--
-- Tests for testing query planner selectivity and width estimates
--
-- Most selectivity and width estimations rely too heavily on statistics
-- gathered by ANALYZE, or could vary depending on hardware.  However, there
-- are a few cases where we can have more certainty about the expected number
-- of rows, or width of rows.  This is a good home for such tests.
--

-- Function to assist with verifying EXPLAIN which includes costs.  A series
-- of bool flags allows control over which portions are masked out
CREATE FUNCTION explain_mask_costs(query text, do_analyze bool,
    hide_costs bool, hide_row_est bool, hide_width bool) RETURNS setof text
LANGUAGE plpgsql AS
$$
DECLARE
    ln text;
    analyze_str text;
BEGIN
    IF do_analyze = true THEN
        analyze_str := 'on';
    ELSE
        analyze_str := 'off';
    END IF;

    -- avoid jit related output by disabling it
    SET LOCAL jit = 0;

    FOR ln IN
        EXECUTE format('explain (analyze %s, costs on, summary off, timing off, buffers off) %s',
            analyze_str, query)
    LOOP
        -- ignore Storage output because the size varies depending on platform
        CONTINUE WHEN ln ~ '^\s*Storage: ';

        IF hide_costs = true THEN
            ln := regexp_replace(ln, 'cost=\d+\.\d\d\.\.\d+\.\d\d', 'cost=N..N');
        END IF;

        IF hide_row_est = true THEN
            -- don't use 'g' so that we leave the actual rows intact
            ln := regexp_replace(ln, 'rows=\d+', 'rows=N');
        END IF;

        IF hide_width = true THEN
            ln := regexp_replace(ln, 'width=\d+', 'width=N');
        END IF;

        RETURN NEXT ln;
    END LOOP;
END;
$$;

--
-- Test the SupportRequestRows support function for generate_series_timestamp()
--

-- Ensure the row estimate matches the actual rows
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '2024-02-01', TIMESTAMPTZ '2024-03-01', INTERVAL '1 day') g(s);$$,
true, true, false, true);

-- As above but with generate_series_timestamp
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMP '2024-02-01', TIMESTAMP '2024-03-01', INTERVAL '1 day') g(s);$$,
true, true, false, true);

-- As above but with generate_series_timestamptz_at_zone()
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '2024-02-01', TIMESTAMPTZ '2024-03-01', INTERVAL '1 day', 'UTC') g(s);$$,
true, true, false, true);

-- Ensure the estimated and actual row counts match when the range isn't
-- evenly divisible by the step
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '2024-02-01', TIMESTAMPTZ '2024-03-01', INTERVAL '7 day') g(s);$$,
true, true, false, true);

-- Ensure the estimates match when step is decreasing
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '2024-03-01', TIMESTAMPTZ '2024-02-01', INTERVAL '-1 day') g(s);$$,
true, true, false, true);

-- Ensure an empty range estimates 1 row
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '2024-03-01', TIMESTAMPTZ '2024-02-01', INTERVAL '1 day') g(s);$$,
true, true, false, true);

-- Ensure we get the default row estimate for infinity values
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '-infinity', TIMESTAMPTZ 'infinity', INTERVAL '1 day') g(s);$$,
false, true, false, true);

-- Ensure the row estimate behaves correctly when step size is zero.
-- We expect generate_series_timestamp() to throw the error rather than in
-- the support function.
SELECT * FROM generate_series(TIMESTAMPTZ '2024-02-01', TIMESTAMPTZ '2024-03-01', INTERVAL '0 day') g(s);

--
-- Test the SupportRequestRows support function for generate_series_numeric()
--

-- Ensure the row estimate matches the actual rows
SELECT explain_mask_costs($$
SELECT * FROM generate_series(1.0, 25.0) g(s);$$,
true, true, false, true);

-- As above but with non-default step
SELECT explain_mask_costs($$
SELECT * FROM generate_series(1.0, 25.0, 2.0) g(s);$$,
true, true, false, true);

-- Ensure the estimates match when step is decreasing
SELECT explain_mask_costs($$
SELECT * FROM generate_series(25.0, 1.0, -1.0) g(s);$$,
true, true, false, true);

-- Ensure an empty range estimates 1 row
SELECT explain_mask_costs($$
SELECT * FROM generate_series(25.0, 1.0, 1.0) g(s);$$,
true, true, false, true);

-- Ensure we get the default row estimate for error cases (infinity/NaN values
-- and zero step size)
SELECT explain_mask_costs($$
SELECT * FROM generate_series('-infinity'::NUMERIC, 'infinity'::NUMERIC, 1.0) g(s);$$,
false, true, false, true);

SELECT explain_mask_costs($$
SELECT * FROM generate_series(1.0, 25.0, 'NaN'::NUMERIC) g(s);$$,
false, true, false, true);

SELECT explain_mask_costs($$
SELECT * FROM generate_series(25.0, 2.0, 0.0) g(s);$$,
false, true, false, true);

--
-- Test ScalarArrayOpExpr row estimates for <> ALL for arrays with NULLs.  We
-- expect the planner to estimate 1 row will match in both of the following
-- tests.
--

-- Try a const array containing a NULL
SELECT explain_mask_costs($$
SELECT * FROM tenk1 WHERE unique1 <> ALL (ARRAY[1, 2, 99, NULL]);$$,
false, true, false, true);

-- Try a non-const array containing a NULL
SELECT explain_mask_costs($$
SELECT * FROM tenk1 WHERE unique1 <> ALL (ARRAY[1, 2, 98, (SELECT 99), NULL]);$$,
false, true, false, true);

-- Verify that scalarineqsel() works on "char" columns
CREATE TEMP TABLE char_table_1 AS
  SELECT i::"char" AS c FROM generate_series(64,96) i;
ANALYZE char_table_1;
EXPLAIN (COSTS OFF) SELECT * FROM char_table_1 WHERE c < 'Q';

-- Test inequality and NULL-safe equality join estimates with NULLs and values
-- outside the MCV lists.  SEMI/ANTI estimates measure existence of a partner.
CREATE TEMP TABLE distinct_outer AS
SELECT CASE WHEN g <= 20 THEN NULL WHEN g <= 80 THEN 1 ELSE 2 END AS a,
       g AS tail, g % 40 AS many,
       CASE WHEN g <= 20 THEN NULL ELSE g END AS nullable_tail
FROM generate_series(1, 100) g;
CREATE TEMP TABLE distinct_inner AS
SELECT 1 AS one, NULL::integer AS nulls, g % 2 + 1 AS two,
       (g % 40)::bigint AS many, g AS tail
FROM generate_series(1, 100) g;
ANALYZE distinct_outer;
ANALYZE distinct_inner;

-- Ordinary <> excludes pairs containing NULL from its equality complement.
SELECT explain_mask_costs($$
SELECT FROM distinct_outer x, distinct_outer y WHERE x.a <> y.a;$$,
true, true, false, true);
SELECT explain_mask_costs($$
SELECT FROM distinct_outer x, distinct_inner y WHERE y.nulls <> x.a;$$,
true, true, false, true);

-- Count NULL/NULL pairs even when non-NULL values are outside the MCV lists.
SELECT explain_mask_costs($$
SELECT FROM distinct_outer x, distinct_outer y
WHERE x.nullable_tail IS NOT DISTINCT FROM y.nullable_tail;$$,
true, true, false, true);
SELECT explain_mask_costs($$
SELECT FROM distinct_outer x, distinct_outer y
WHERE x.nullable_tail IS DISTINCT FROM y.nullable_tail;$$,
true, true, false, true);

-- NULL matches must not suppress non-MCV matches in a NULL-safe SEMI join.
SELECT explain_mask_costs($$
SELECT x.nullable_tail FROM distinct_outer x WHERE EXISTS
  (SELECT FROM distinct_outer y
   WHERE x.nullable_tail IS NOT DISTINCT FROM y.nullable_tail);$$,
true, true, false, true);

-- Outer NULLs have no NULL-safe equality partner in a non-NULL inner column.
SELECT explain_mask_costs($$
SELECT x.nullable_tail FROM distinct_outer x WHERE NOT EXISTS
  (SELECT FROM distinct_inner y
   WHERE x.nullable_tail IS NOT DISTINCT FROM y.tail);$$,
true, true, false, true);

-- All-NULL inner data matches only the outer NULLs, even without MCV lists.
SELECT explain_mask_costs($$
SELECT x.nullable_tail FROM distinct_outer x WHERE EXISTS
  (SELECT FROM distinct_inner y
   WHERE x.nullable_tail IS NOT DISTINCT FROM y.nulls);$$,
true, true, false, true);

-- A sole inner value gives SEMI unequal outer rows and ANTI equal outer rows.
SELECT explain_mask_costs($$
SELECT x.a FROM distinct_outer x WHERE EXISTS
  (SELECT FROM distinct_inner y WHERE x.a IS DISTINCT FROM y.one);$$,
true, true, false, true);
SELECT explain_mask_costs($$
SELECT x.a FROM distinct_outer x WHERE NOT EXISTS
  (SELECT FROM distinct_inner y WHERE y.one IS DISTINCT FROM x.a);$$,
true, true, false, true);

-- With an all-NULL inner input, DISTINCT ANTI keeps only outer NULLs.
SELECT explain_mask_costs($$
SELECT x.a FROM distinct_outer x WHERE NOT EXISTS
  (SELECT FROM distinct_inner y WHERE x.a IS DISTINCT FROM y.nulls);$$,
true, true, false, true);

-- Two different inner values provide a DISTINCT partner for every outer row.
-- Zero output rows are estimated as one because of clamp_row_est().
SELECT explain_mask_costs($$
SELECT x.a FROM distinct_outer x WHERE NOT EXISTS
  (SELECT FROM distinct_inner y WHERE x.a IS DISTINCT FROM y.two);$$,
true, true, false, true);

-- All outer values are outside the MCV list; one equals the sole inner value.
SELECT explain_mask_costs($$
SELECT x.tail FROM distinct_outer x WHERE EXISTS
  (SELECT FROM distinct_inner y WHERE x.tail IS DISTINCT FROM y.one);$$,
true, true, false, true);

-- Cross-type MCV lists exercise reversed operands and hashing in assertion builds.
SELECT explain_mask_costs($$
SELECT x.many FROM distinct_outer x WHERE NOT EXISTS
  (SELECT FROM distinct_inner y WHERE y.many IS DISTINCT FROM x.many);$$,
true, true, false, true);

-- Ordinary <> uses the same existence estimate but cannot match either NULL.
-- A sole inner value matches only unequal non-NULL outer rows.
SELECT explain_mask_costs($$
SELECT x.a FROM distinct_outer x WHERE EXISTS
  (SELECT FROM distinct_inner y WHERE x.a <> y.one);$$,
true, true, false, true);

-- An all-NULL inner input has no <> partners, so ANTI keeps all outer rows.
SELECT explain_mask_costs($$
SELECT x.a FROM distinct_outer x WHERE NOT EXISTS
  (SELECT FROM distinct_inner y WHERE x.a <> y.nulls);$$,
true, true, false, true);

-- Two non-NULL inner values match every non-NULL outer row, leaving NULLs to ANTI.
SELECT explain_mask_costs($$
SELECT x.a FROM distinct_outer x WHERE NOT EXISTS
  (SELECT FROM distinct_inner y WHERE x.a <> y.two);$$,
true, true, false, true);

-- With one inner row, use the non-NULL pair frequency minus equality.
SELECT explain_mask_costs($$
SELECT x.a FROM distinct_outer x WHERE EXISTS
  (SELECT FROM (SELECT one FROM distinct_inner LIMIT 1) y WHERE x.a <> y.one);$$,
true, true, false, true);

-- Unrecognized estimators keep their five-argument API and legacy estimates.
-- Both operators may use the same estimator, as some extensions do.
CREATE SCHEMA distinct_est;
CREATE OPERATOR distinct_est.= (
  LEFTARG = integer, RIGHTARG = integer,
  FUNCTION = pg_catalog.int4eq, NEGATOR = OPERATOR(distinct_est.<>),
  JOIN = pg_catalog.contjoinsel);
CREATE OPERATOR distinct_est.<> (
  LEFTARG = integer, RIGHTARG = integer,
  FUNCTION = pg_catalog.int4ne, NEGATOR = OPERATOR(distinct_est.=),
  JOIN = pg_catalog.contjoinsel);
SET search_path = distinct_est, public, pg_catalog;
SELECT explain_mask_costs($$
SELECT FROM distinct_outer x, distinct_inner y
WHERE x.tail IS DISTINCT FROM y.tail;$$,
false, true, false, true);
SELECT explain_mask_costs($$
SELECT FROM distinct_outer x, distinct_inner y
WHERE x.tail IS NOT DISTINCT FROM y.tail;$$,
false, true, false, true);

-- An alias of eqjoinsel is still a separate, five-argument catalog procedure.
CREATE FUNCTION distinct_est.join_est(internal, oid, internal, smallint, internal)
  RETURNS double precision AS 'eqjoinsel' LANGUAGE internal STRICT;
ALTER OPERATOR distinct_est.= (integer, integer)
  SET (JOIN = distinct_est.join_est);
SELECT explain_mask_costs($$
SELECT FROM distinct_outer x, distinct_outer y
WHERE x.a IS NOT DISTINCT FROM y.a;$$,
false, true, false, true);
RESET search_path;
DROP OPERATOR distinct_est.= (integer, integer);
DROP OPERATOR distinct_est.<> (integer, integer);
DROP FUNCTION distinct_est.join_est(internal, oid, internal, smallint, internal);
DROP SCHEMA distinct_est;

DROP TABLE distinct_outer, distinct_inner;

DROP FUNCTION explain_mask_costs(text, bool, bool, bool, bool);
