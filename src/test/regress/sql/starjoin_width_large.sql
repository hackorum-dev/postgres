--
-- STARJOIN_WIDTH_LARGE
-- Same as starjoin_width, but with larger tables (expensive)
--

-- Ignore platform-dependent memory and hash sizing, but retain the
-- distinction between single-batch and multi-batch hashing.
CREATE OR REPLACE FUNCTION pg_temp.starjoin_explain(query text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
  ln text;
BEGIN
  FOR ln IN EXECUTE
    'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, BUFFERS OFF, SUMMARY OFF) ' || query
  LOOP
    ln := regexp_replace(ln, '\m\d+kB', 'NkB', 'g');
    ln := regexp_replace(ln, 'Buckets: \d+', 'Buckets: N');
    ln := regexp_replace(ln, 'Batches: (?!1\M)\d+', 'Batches: multiple');
    ln := regexp_replace(ln, ' \(originally \d+\)', '', 'g');
    RETURN NEXT ln;
  END LOOP;
END;
$$;

CREATE TABLE starjoin_width_wa (id int PRIMARY KEY, t text);
CREATE TABLE starjoin_width_wb (id int PRIMARY KEY, t text);
CREATE TABLE starjoin_width_wc (id int PRIMARY KEY, v int);

INSERT INTO starjoin_width_wa SELECT i, repeat(md5(i::text), 16) FROM generate_series(1,100) i;
INSERT INTO starjoin_width_wb SELECT i, repeat(md5(i::text), 16) FROM generate_series(1,100) i;
INSERT INTO starjoin_width_wc SELECT i, i FROM generate_series(1,100000) i;

CREATE TABLE starjoin_width_wf (id int, a int NOT NULL REFERENCES starjoin_width_wa, b int NOT NULL REFERENCES starjoin_width_wb, c int NOT NULL REFERENCES starjoin_width_wc);

INSERT INTO starjoin_width_wf SELECT i, 1 + i % 100, 1 + (i * 7) % 100, 1 + (i * 13) % 100000 FROM generate_series(1,100000) i;

VACUUM ANALYZE starjoin_width_wa, starjoin_width_wb, starjoin_width_wc, starjoin_width_wf;

SET max_parallel_workers_per_gather = 0;
-- Use a small work_mem, so that the hash join to wc needs multiple batches.
SET work_mem = '400kB';
SET enable_nestloop = off; SET enable_mergejoin = off;

-- All the joins preserve the cardinality, but the cost of a batched hash join
-- depends on the width of the outer rows. So when the hash join to wc needs
-- multiple batches (as in starjoin_width_large), the plan with the GUC off
-- joins wc before the wide dimensions (wa, wb), while the canonical order
-- joins it last. Otherwise the plans are the same.

-- pure FK joins, GUC on
SELECT pg_temp.starjoin_explain($$
SELECT wa.t, wb.t, wc.v FROM starjoin_width_wf AS wf JOIN starjoin_width_wa AS wa ON wf.a = wa.id JOIN starjoin_width_wb AS wb ON wf.b = wb.id JOIN starjoin_width_wc AS wc ON wf.c = wc.id
$$) AS "QUERY PLAN";

-- pure FK joins, GUC off
SET enable_starjoin_ordering = off;
SELECT pg_temp.starjoin_explain($$
SELECT wa.t, wb.t, wc.v FROM starjoin_width_wf AS wf JOIN starjoin_width_wa AS wa ON wf.a = wa.id JOIN starjoin_width_wb AS wb ON wf.b = wb.id JOIN starjoin_width_wc AS wc ON wf.c = wc.id
$$) AS "QUERY PLAN";

DROP TABLE starjoin_width_wf, starjoin_width_wa, starjoin_width_wb, starjoin_width_wc;
