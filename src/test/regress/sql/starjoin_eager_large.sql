--
-- STARJOIN_EAGER_LARGE
-- Same as starjoin_eager, but with larger tables (expensive)
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

CREATE TABLE starjoin_eager_h1 (id int PRIMARY KEY, v int);
CREATE TABLE starjoin_eager_h2 (id int PRIMARY KEY, v int);
CREATE TABLE starjoin_eager_h3 (id int PRIMARY KEY, v int);

INSERT INTO starjoin_eager_h1 SELECT i, i % 10 FROM generate_series(1,100) i;
INSERT INTO starjoin_eager_h2 SELECT i, i FROM generate_series(1,100000) i;
INSERT INTO starjoin_eager_h3 SELECT i, i FROM generate_series(1,100000) i;

CREATE TABLE starjoin_eager_hf (id int, d1 int NOT NULL, d2 int NOT NULL, d3 int NOT NULL, x int);
INSERT INTO starjoin_eager_hf SELECT i, 1 + i % 100, 1 + (i * 7) % 100000, 1 + (i * 13) % 100000, i % 1000
  FROM generate_series(1,200000) i;

ALTER TABLE starjoin_eager_hf
  ADD FOREIGN KEY (d1) REFERENCES starjoin_eager_h1,
  ADD FOREIGN KEY (d2) REFERENCES starjoin_eager_h2,
  ADD FOREIGN KEY (d3) REFERENCES starjoin_eager_h3;

VACUUM ANALYZE starjoin_eager_h1, starjoin_eager_h2,
               starjoin_eager_h3, starjoin_eager_hf;

SET max_parallel_workers_per_gather = 0;
-- Use a small work_mem, so that the hash joins to h2 and h3 need multiple
-- batches.
SET work_mem = '400kB';

-- With the GUC on, hf is never joined to h2 and h3 before h1, the first
-- dimension in the canonical order, so the partial aggregation (grouping by
-- hf.d1) can't be done below the join to h1, unlike with the GUC off.

-- eager agg, GROUP BY first dim (h1), GUC on
SET enable_starjoin_ordering = on;
SELECT pg_temp.starjoin_explain($$
SELECT h1.v, sum(hf.x)
FROM starjoin_eager_hf AS hf JOIN starjoin_eager_h1 AS h1 ON hf.d1 = h1.id
                             JOIN starjoin_eager_h2 AS h2 ON hf.d2 = h2.id
                             JOIN starjoin_eager_h3 AS h3 ON hf.d3 = h3.id
GROUP BY h1.v
$$) AS "QUERY PLAN";

-- eager agg, GROUP BY first dim (h1), GUC off
SET enable_starjoin_ordering = off;
SELECT pg_temp.starjoin_explain($$
SELECT h1.v, sum(hf.x)
FROM starjoin_eager_hf AS hf JOIN starjoin_eager_h1 AS h1 ON hf.d1 = h1.id
                             JOIN starjoin_eager_h2 AS h2 ON hf.d2 = h2.id
                             JOIN starjoin_eager_h3 AS h3 ON hf.d3 = h3.id
GROUP BY h1.v
$$) AS "QUERY PLAN";

-- With h1 last in FROM, and so also last in the canonical order, the partial
-- aggregation is done below the join to h1 with the GUC on too.

-- same query, h1 written last in FROM, GUC on
SET enable_starjoin_ordering = on;
SELECT pg_temp.starjoin_explain($$
SELECT h1.v, sum(hf.x)
FROM starjoin_eager_hf AS hf JOIN starjoin_eager_h2 AS h2 ON hf.d2 = h2.id
                             JOIN starjoin_eager_h3 AS h3 ON hf.d3 = h3.id
                             JOIN starjoin_eager_h1 AS h1 ON hf.d1 = h1.id
GROUP BY h1.v
$$) AS "QUERY PLAN";

-- same query, h1 written last in FROM, GUC off
SET enable_starjoin_ordering = off;
SELECT pg_temp.starjoin_explain($$
SELECT h1.v, sum(hf.x)
FROM starjoin_eager_hf AS hf JOIN starjoin_eager_h2 AS h2 ON hf.d2 = h2.id
                             JOIN starjoin_eager_h3 AS h3 ON hf.d3 = h3.id
                             JOIN starjoin_eager_h1 AS h1 ON hf.d1 = h1.id
GROUP BY h1.v
$$) AS "QUERY PLAN";

DROP TABLE starjoin_eager_hf, starjoin_eager_h1, starjoin_eager_h2, starjoin_eager_h3;
