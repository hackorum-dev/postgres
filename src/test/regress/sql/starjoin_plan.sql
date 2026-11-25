--
-- STARJOIN_PLAN
-- Test star join ordering with extra join quals between fact and dimension
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

CREATE TABLE starjoin_plan_p1 (id int PRIMARY KEY, v int, w int);
CREATE TABLE starjoin_plan_p2 (id int PRIMARY KEY, v int, w int);
CREATE TABLE starjoin_plan_p3 (id int PRIMARY KEY, v int, w int);

INSERT INTO starjoin_plan_p1 SELECT i, i % 100, i FROM generate_series(1,1000) i;
INSERT INTO starjoin_plan_p2 SELECT i, i % 100, i FROM generate_series(1,1000) i;
INSERT INTO starjoin_plan_p3 SELECT i, i % 100, i FROM generate_series(1,1000) i;

CREATE TABLE starjoin_plan_pf (id int PRIMARY KEY,
  d1 int NOT NULL REFERENCES starjoin_plan_p1,
  d2 int NOT NULL REFERENCES starjoin_plan_p2,
  d3 int NOT NULL REFERENCES starjoin_plan_p3,
  x int);

INSERT INTO starjoin_plan_pf SELECT i, 1 + (i * 7) % 1000, 1 + (i * 13) % 1000, 1 + (i * 17) % 1000, i % 1000
  FROM generate_series(1,20000) i;

VACUUM ANALYZE starjoin_plan_p1, starjoin_plan_p2, starjoin_plan_p3, starjoin_plan_pf;

SET max_parallel_workers_per_gather = 0;

-- Q1: extra (non-FK) join qual between fact and the last dimension. It makes
-- the join to p3 selective, but p3 is still a dimension, so with the GUC on
-- p3 is joined last, while with the GUC off it's joined first.

-- join the fact to p1, p2 and p3, with an extra join qual for p3, GUC on
SET enable_starjoin_ordering = on;
SELECT pg_temp.starjoin_explain($$
SELECT count(*) FROM starjoin_plan_pf AS pf
  JOIN starjoin_plan_p1 AS p1 ON pf.d1 = p1.id
  JOIN starjoin_plan_p2 AS p2 ON pf.d2 = p2.id
  JOIN starjoin_plan_p3 AS p3 ON pf.d3 = p3.id AND pf.x = p3.w
$$) AS "QUERY PLAN";

-- same query, GUC off
SET enable_starjoin_ordering = off;
SELECT pg_temp.starjoin_explain($$
SELECT count(*) FROM starjoin_plan_pf AS pf
  JOIN starjoin_plan_p1 AS p1 ON pf.d1 = p1.id
  JOIN starjoin_plan_p2 AS p2 ON pf.d2 = p2.id
  JOIN starjoin_plan_p3 AS p3 ON pf.d3 = p3.id AND pf.x = p3.w
$$) AS "QUERY PLAN";

-- Q2: OR clause from which extract_restriction_or_clauses() derives a
-- restriction on p3, so p3 is not a dimension (the clusters are built after
-- deriving the restrictions), and the plan is the same as with the GUC off.

-- join the fact to p1, p2 and p3, with an OR clause in WHERE, GUC on
SET enable_starjoin_ordering = on;
SELECT pg_temp.starjoin_explain($$
SELECT count(*) FROM starjoin_plan_pf AS pf
  JOIN starjoin_plan_p1 AS p1 ON pf.d1 = p1.id
  JOIN starjoin_plan_p2 AS p2 ON pf.d2 = p2.id
  JOIN starjoin_plan_p3 AS p3 ON pf.d3 = p3.id
 WHERE (p3.w = 1 AND pf.x = 1) OR (p3.w = 2 AND pf.x = 2)
$$) AS "QUERY PLAN";

-- same query, GUC off
SET enable_starjoin_ordering = off;
SELECT pg_temp.starjoin_explain($$
SELECT count(*) FROM starjoin_plan_pf AS pf
  JOIN starjoin_plan_p1 AS p1 ON pf.d1 = p1.id
  JOIN starjoin_plan_p2 AS p2 ON pf.d2 = p2.id
  JOIN starjoin_plan_p3 AS p3 ON pf.d3 = p3.id
 WHERE (p3.w = 1 AND pf.x = 1) OR (p3.w = 2 AND pf.x = 2)
$$) AS "QUERY PLAN";

DROP TABLE starjoin_plan_pf, starjoin_plan_p1, starjoin_plan_p2, starjoin_plan_p3;
