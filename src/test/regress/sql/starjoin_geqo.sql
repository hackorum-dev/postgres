--
-- Checks for star queries and GEQO with enable_starjoin_ordering
--
-- The standard join search joins the dimensions of a star to the fact only
-- in the canonical order, so it considers only d+1 of the 2^d subsets of the
-- d dimensions.  So the dimensions count as only ceil(log2(d+1)) rels toward
-- geqo_threshold, and a star gets planned by the standard join search even
-- if it has more rels than geqo_threshold.  With enough other rels GEQO is
-- still used, and the pruning does not make it any cheaper, because GEQO
-- evaluates a fixed number of join orders, each joining all the rels.
-- Checking the canonical order only adds overhead.
--
-- Each check is a single boolean, phrased as the desired behavior.  EXPLAIN
-- does not show whether a query was planned by GEQO, so the checks detect
-- GEQO by making it fail (see starjoin_geqo_used).
--

SET max_parallel_workers_per_gather = 0;

-- the plan (without costs), with enable_starjoin_ordering enabled/disabled
CREATE FUNCTION starjoin_geqo_plan(query text, starjoin bool) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
  plan json;
BEGIN
  PERFORM set_config('enable_starjoin_ordering', starjoin::text, true);
  EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || query INTO plan;
  RETURN plan::text;
END;
$$;

-- is the query planned by GEQO? (with enable_starjoin_ordering
-- enabled/disabled)  GEQO fails to allocate a population this large, while
-- the standard join search ignores geqo_pool_size.
CREATE FUNCTION starjoin_geqo_used(query text, starjoin bool) RETURNS bool
LANGUAGE plpgsql SET geqo_pool_size = 100000000 AS $$
BEGIN
  PERFORM set_config('enable_starjoin_ordering', starjoin::text, true);
  EXECUTE 'EXPLAIN ' || query;
  RETURN false;
EXCEPTION WHEN internal_error THEN
  IF SQLERRM NOT LIKE 'invalid memory alloc request size %' THEN
    RAISE;
  END IF;
  RETURN true;
END;
$$;

-- star with 10 dimensions d1-d10 and a table "a" referenced by the fact (an
-- 11th dimension), and a table c1 joined to the fact on its primary key (not
-- a dimension, as there's no foreign key)
CREATE TABLE starjoin_geqo_a (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off);
INSERT INTO starjoin_geqo_a SELECT i, i FROM generate_series(1, 100) i;

CREATE TABLE starjoin_geqo_c1 (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off);
INSERT INTO starjoin_geqo_c1 SELECT i, i FROM generate_series(1, 100) i;

DO $$
BEGIN
  FOR i IN 1..10 LOOP
    EXECUTE format('CREATE TABLE starjoin_geqo_d%s (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off)', i);
    EXECUTE format('INSERT INTO starjoin_geqo_d%s SELECT i, i FROM generate_series(1, 100) i', i);
  END LOOP;
END;
$$;

CREATE TABLE starjoin_geqo_f (id int PRIMARY KEY, a int NOT NULL,
  d1 int NOT NULL, d2 int NOT NULL, d3 int NOT NULL, d4 int NOT NULL,
  d5 int NOT NULL, d6 int NOT NULL, d7 int NOT NULL, d8 int NOT NULL,
  d9 int NOT NULL, d10 int NOT NULL) WITH (autovacuum_enabled = off);

INSERT INTO starjoin_geqo_f
SELECT i, 1 + i % 100, 1 + (i * 3) % 100, 1 + (i * 7) % 100, 1 + (i * 11) % 100,
       1 + (i * 13) % 100, 1 + (i * 17) % 100, 1 + (i * 19) % 100,
       1 + (i * 23) % 100, 1 + (i * 29) % 100, 1 + (i * 31) % 100,
       1 + (i * 37) % 100
  FROM generate_series(1, 1000) i;

-- add the foreign keys after loading the data (faster than checking each row)
ALTER TABLE starjoin_geqo_f
  ADD FOREIGN KEY (a) REFERENCES starjoin_geqo_a,
  ADD FOREIGN KEY (d1) REFERENCES starjoin_geqo_d1,
  ADD FOREIGN KEY (d2) REFERENCES starjoin_geqo_d2,
  ADD FOREIGN KEY (d3) REFERENCES starjoin_geqo_d3,
  ADD FOREIGN KEY (d4) REFERENCES starjoin_geqo_d4,
  ADD FOREIGN KEY (d5) REFERENCES starjoin_geqo_d5,
  ADD FOREIGN KEY (d6) REFERENCES starjoin_geqo_d6,
  ADD FOREIGN KEY (d7) REFERENCES starjoin_geqo_d7,
  ADD FOREIGN KEY (d8) REFERENCES starjoin_geqo_d8,
  ADD FOREIGN KEY (d9) REFERENCES starjoin_geqo_d9,
  ADD FOREIGN KEY (d10) REFERENCES starjoin_geqo_d10;

VACUUM ANALYZE starjoin_geqo_a, starjoin_geqo_c1, starjoin_geqo_d1,
  starjoin_geqo_d2, starjoin_geqo_d3, starjoin_geqo_d4, starjoin_geqo_d5,
  starjoin_geqo_d6, starjoin_geqo_d7, starjoin_geqo_d8, starjoin_geqo_d9,
  starjoin_geqo_d10, starjoin_geqo_f;

--
-- Star with explicit joins
--
-- With join_collapse_limit = 20 the star with 10 dimensions (11 rels) is a
-- single subproblem, above geqo_threshold = 8.  But the dimensions count as
-- only 4 rels, so the star is planned by the standard join search, which
-- joins the dimensions in the canonical order.
--
SELECT $$
SELECT count(*) FROM starjoin_geqo_f AS f
  JOIN starjoin_geqo_d1 AS d1 ON f.d1 = d1.id
  JOIN starjoin_geqo_d2 AS d2 ON f.d2 = d2.id
  JOIN starjoin_geqo_d3 AS d3 ON f.d3 = d3.id
  JOIN starjoin_geqo_d4 AS d4 ON f.d4 = d4.id
  JOIN starjoin_geqo_d5 AS d5 ON f.d5 = d5.id
  JOIN starjoin_geqo_d6 AS d6 ON f.d6 = d6.id
  JOIN starjoin_geqo_d7 AS d7 ON f.d7 = d7.id
  JOIN starjoin_geqo_d8 AS d8 ON f.d8 = d8.id
  JOIN starjoin_geqo_d9 AS d9 ON f.d9 = d9.id
  JOIN starjoin_geqo_d10 AS d10 ON f.d10 = d10.id
$$ AS q_star \gset

-- the plan with the canonical order, forced by join_collapse_limit = 1
SET join_collapse_limit = 1;
SELECT starjoin_geqo_plan(:'q_star', true) AS plan_canonical \gset

SET join_collapse_limit = 20;
SET geqo_threshold = 8;
-- the star is planned by the standard join search, not by GEQO
SELECT NOT starjoin_geqo_used(:'q_star', true) AS star_standard_search;
-- the standard join search uses the canonical order
SELECT starjoin_geqo_plan(:'q_star', true) = :'plan_canonical' AS star_plan_canonical;

-- An additional join clause excludes d10 from the cluster.  The other nine
-- dimensions count as four rels, so with f and d10 the count is six.
SET geqo_threshold = 6;
SELECT starjoin_geqo_used(:'q_star' || ' AND f.id > d10.v', true)
       AS selective_dimension_geqo;

-- Sampling d10 also excludes it from the cluster, giving the same count.
SELECT starjoin_geqo_used(
         replace(:'q_star', 'starjoin_geqo_d10 AS d10',
                 'starjoin_geqo_d10 AS d10 TABLESAMPLE BERNOULLI (10)'), true)
       AS bernoulli_dimension_geqo;
SELECT starjoin_geqo_used(
         replace(:'q_star', 'starjoin_geqo_d10 AS d10',
                 'starjoin_geqo_d10 AS d10 TABLESAMPLE SYSTEM (10)'), true)
       AS system_dimension_geqo;

-- Sampling the fact does not disqualify its unsampled dimensions.
SELECT NOT starjoin_geqo_used(
         replace(:'q_star', 'starjoin_geqo_f AS f',
                 'starjoin_geqo_f AS f TABLESAMPLE BERNOULLI (10)'), true)
       AS sampled_fact_standard_search;
RESET geqo_threshold;
RESET join_collapse_limit;

--
-- Star in a FROM list
--
-- A FROM list of tables is never split into subproblems (join_collapse_limit
-- applies to explicit joins, from_collapse_limit to subqueries), so with the
-- default settings a star with 11 dimensions (12 rels) would be planned by
-- GEQO.  But the dimensions count as only 4 rels, so the star is planned by
-- the standard join search.  With enough other rels GEQO is still used, and
-- it's pruned too.
--
SELECT $$
SELECT count(*)
  FROM starjoin_geqo_f AS f, starjoin_geqo_a AS a,
       starjoin_geqo_d1 AS d1, starjoin_geqo_d2 AS d2,
       starjoin_geqo_d3 AS d3, starjoin_geqo_d4 AS d4,
       starjoin_geqo_d5 AS d5, starjoin_geqo_d6 AS d6,
       starjoin_geqo_d7 AS d7, starjoin_geqo_d8 AS d8,
       starjoin_geqo_d9 AS d9, starjoin_geqo_d10 AS d10
 WHERE f.a = a.id AND f.d1 = d1.id AND f.d2 = d2.id AND f.d3 = d3.id
   AND f.d4 = d4.id AND f.d5 = d5.id AND f.d6 = d6.id AND f.d7 = d7.id
   AND f.d8 = d8.id AND f.d9 = d9.id AND f.d10 = d10.id
$$ AS q_star_list \gset

-- the same star, with another rel (c1) joined to the fact
SELECT $$
SELECT count(*)
  FROM starjoin_geqo_f AS f, starjoin_geqo_a AS a,
       starjoin_geqo_d1 AS d1, starjoin_geqo_d2 AS d2,
       starjoin_geqo_d3 AS d3, starjoin_geqo_d4 AS d4,
       starjoin_geqo_d5 AS d5, starjoin_geqo_d6 AS d6,
       starjoin_geqo_d7 AS d7, starjoin_geqo_d8 AS d8,
       starjoin_geqo_d9 AS d9, starjoin_geqo_d10 AS d10,
       starjoin_geqo_c1 AS c1
 WHERE f.a = a.id AND f.d1 = d1.id AND f.d2 = d2.id AND f.d3 = d3.id
   AND f.d4 = d4.id AND f.d5 = d5.id AND f.d6 = d6.id AND f.d7 = d7.id
   AND f.d8 = d8.id AND f.d9 = d9.id AND f.d10 = d10.id AND f.id = c1.id
$$ AS q_star_list_c1 \gset

-- with the GUC disabled, GEQO is used as usual
SELECT starjoin_geqo_used(:'q_star_list', false) AS star_list_geqo_off;
-- the dimensions count as fewer rels toward geqo_threshold
SELECT NOT starjoin_geqo_used(:'q_star_list', true) AS star_list_standard_search;

-- the 11 dimensions count as 4 rels, so with f and c1 the star counts as 6
SET geqo_threshold = 7;
SELECT NOT starjoin_geqo_used(:'q_star_list_c1', true) AS star_list_c1_standard_search;
SET geqo_threshold = 6;
SELECT starjoin_geqo_used(:'q_star_list_c1', true) AS star_list_c1_geqo;
-- XXX GEQO is pruned too, so the GUC changes the plan
SELECT starjoin_geqo_plan(:'q_star_list_c1', true)
       = starjoin_geqo_plan(:'q_star_list_c1', false) AS geqo_plan_unaffected;
RESET geqo_threshold;

RESET max_parallel_workers_per_gather;

DROP FUNCTION starjoin_geqo_plan(text, bool);
DROP FUNCTION starjoin_geqo_used(text, bool);

DROP TABLE starjoin_geqo_f, starjoin_geqo_a, starjoin_geqo_c1,
  starjoin_geqo_d1, starjoin_geqo_d2, starjoin_geqo_d3, starjoin_geqo_d4,
  starjoin_geqo_d5, starjoin_geqo_d6, starjoin_geqo_d7, starjoin_geqo_d8,
  starjoin_geqo_d9, starjoin_geqo_d10;
