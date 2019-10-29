/* skip test if JIT is not available */
SELECT NOT (pg_jit_available() AND current_setting('jit')::bool)
       AS skip_test \gset
\if :skip_test
\quit
\endif

-- start with a known baseline
set jit_expressions = true;
set jit_tuple_deforming = true;
-- to reliably test, despite costs varying between platforms
set jit_above_cost = 0;
-- to make the bulk of the test cheaper
set jit_optimize_above_cost = -1;
set jit_inline_above_cost = -1;

CREATE TABLE jittest_simple(id serial primary key, data text);
INSERT INTO jittest_simple(data) VALUES('row1');
INSERT INTO jittest_simple(data) VALUES('row2');

-- verify that a simple relation-less query can be JITed
BEGIN;
SET LOCAL jit = false;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT txid_current() = txid_current();
SELECT txid_current() = txid_current();
COMMIT;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT txid_current() = txid_current();
SELECT txid_current() = txid_current();


-- that tuple deforming for a plain seqscan is JITed when projecting
BEGIN;
SET LOCAL jit = false;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT data FROM jittest_simple;
SELECT data FROM jittest_simple;
COMMIT;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT data FROM jittest_simple;
SELECT data FROM jittest_simple;

-- unfortunately currently the physical tlist optimization may prevent
-- JITed tuple deforming from taking effect
BEGIN;
SET LOCAL jit = false;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT * FROM jittest_simple;
SELECT * FROM jittest_simple;
COMMIT;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT * FROM jittest_simple;
SELECT * FROM jittest_simple;

-- check that tuple deforming on wide tables works
BEGIN;
SET LOCAL jit_tuple_deforming = false;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT firstc, lastc FROM extra_wide_table;
SELECT firstc, lastc FROM extra_wide_table;
COMMIT;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT firstc, lastc FROM extra_wide_table;
SELECT firstc, lastc FROM extra_wide_table;

-----
-- test costing
-----

-- don't perform JIT compilation unless worthwhile
BEGIN;
SET LOCAL jit_above_cost = 8000000000;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT tableoid FROM jittest_simple;
SET LOCAL enable_seqscan = false;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT tableoid FROM jittest_simple;
COMMIT;

-- optimize once expensive enough
BEGIN;
SET LOCAL jit_above_cost = 0;
SET LOCAL jit_optimize_above_cost = 8000000000;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT tableoid FROM jittest_simple;
SET LOCAL enable_seqscan = false;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT tableoid FROM jittest_simple;
COMMIT;

-- behave sanely if optimization cost is below general JIT costs
BEGIN;
SET LOCAL jit_above_cost = 8000000000;
SET LOCAL jit_optimize_above_cost = 0;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT tableoid FROM jittest_simple;
SET LOCAL enable_seqscan = false;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT tableoid FROM jittest_simple;
COMMIT;

-- perform inlining once expensive enough
BEGIN;
SET LOCAL jit_above_cost = 0;
SET LOCAL jit_inline_above_cost = 8000000000;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT tableoid FROM jittest_simple;
SET LOCAL enable_seqscan = false;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT tableoid FROM jittest_simple;
COMMIT;

-- perform inlining once expensive enough
BEGIN;
SET LOCAL jit_above_cost = 0;
SET LOCAL jit_inline_above_cost = 8000000000;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT tableoid FROM jittest_simple;
SET LOCAL enable_seqscan = false;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT tableoid FROM jittest_simple;
COMMIT;


-- perform inlining and optimization once expensive enough
BEGIN;
SET LOCAL jit_above_cost = 0;
SET LOCAL jit_inline_above_cost = 8000000000;
SET LOCAL jit_optimize_above_cost = 8000000000;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT tableoid FROM jittest_simple;
SET LOCAL enable_seqscan = false;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT tableoid FROM jittest_simple;
COMMIT;

-- check that inner/outer tuple deforming can be inferred for upper nodes, join case
BEGIN;
SET LOCAL enable_hashjoin = true;
SET LOCAL enable_mergejoin = false;
SET LOCAL enable_nestloop = false;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT a.data || b.data FROM jittest_simple a JOIN jittest_simple b USING(id);
SELECT a.data || b.data FROM jittest_simple a JOIN jittest_simple b USING(id);
COMMIT;
BEGIN;
SET LOCAL enable_hashjoin = false;
SET LOCAL enable_mergejoin = true;
SET LOCAL enable_nestloop = false;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT a.data || b.data FROM jittest_simple a JOIN jittest_simple b USING(id);
SELECT a.data || b.data FROM jittest_simple a JOIN jittest_simple b USING(id);
COMMIT;
BEGIN;
SET LOCAL enable_hashjoin = false;
SET LOCAL enable_mergejoin = false;
SET LOCAL enable_nestloop = true;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT a.data || b.data FROM jittest_simple a JOIN jittest_simple b USING(id);
SELECT a.data || b.data FROM jittest_simple a JOIN jittest_simple b USING(id);
COMMIT;

-- check that inner/outer tuple deforming can be inferred for upper nodes, agg case
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT count(*), count(data), string_agg(data, ':') FROM jittest_simple;
SELECT count(*), count(data), string_agg(data, ':') FROM jittest_simple;

-- Check that the equality hash-table function in a hash-aggregate can
-- be accelerated.
BEGIN;
SET LOCAL enable_hashagg = true;
SET LOCAL enable_sort = false;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT data, string_agg(id::text, ', ') FROM jittest_simple GROUP BY data;
SELECT data, string_agg(id::text, ', ') FROM jittest_simple GROUP BY data;
END;

-- Unfortunately for sort based aggregates, the group comparison
-- function can current not be JITed
BEGIN;
SET LOCAL enable_hashagg = false;
SET LOCAL enable_sort = true;
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS) SELECT data, string_agg(id::text, ', ') FROM jittest_simple GROUP BY data;
SELECT data, string_agg(id::text, ', ') FROM jittest_simple GROUP BY data;
END;

-- check that EXPLAIN ANALYZE output is reproducible with the right options
EXPLAIN (VERBOSE, COSTS OFF, JIT_DETAILS, ANALYZE, TIMING OFF, SUMMARY OFF) SELECT tableoid FROM jittest_simple;

DROP TABLE jittest_simple;
