--
-- Checks for star queries with Var-free quals and enable_starjoin_ordering
--
-- A Var-free qual that is not volatile, e.g. "now() > '2000-01-01'", is a
-- pseudoconstant, evaluated only once (in a gating Result node).  But it gets
-- the relids of the whole join domain, so it's in the joininfo list of every
-- rel, and have_relevant_joinclause() finds a join clause between any two
-- rels.  So every dimension fails the check that it has no join clauses to
-- rels other than the fact (see generate_starjoin_clusters), and the query
-- has no starjoin clusters at all.  The dimensions then count fully toward
-- geqo_threshold, and the standard join search does not use the canonical
-- order.
--
-- With enable_starjoin_ordering disabled, the qual makes the standard join
-- search consider all subsets of the rels too, so this is not a regression.
-- The qual also makes the dimensions joinable to each other, so the standard
-- join search would consider all subsets of the dimensions, if joinrels with
-- a dimension but without the fact were allowed (see starjoin_order_invalid).
--
-- Each check is a single boolean, phrased as the desired behavior.  EXPLAIN
-- does not show whether a query was planned by GEQO, so the checks detect
-- GEQO by making it fail (see starjoin_var_free_geqo_used).
--

-- is the query planned by GEQO? (with enable_starjoin_ordering
-- enabled/disabled)  GEQO fails to allocate a population this large, while
-- the standard join search ignores geqo_pool_size.
CREATE FUNCTION starjoin_var_free_geqo_used(query text, starjoin bool) RETURNS bool
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

-- star with 11 dimensions d1-d11
DO $$
BEGIN
  FOR i IN 1..11 LOOP
    EXECUTE format('CREATE TABLE starjoin_var_free_d%s (id int PRIMARY KEY) WITH (autovacuum_enabled = off)', i);
    EXECUTE format('INSERT INTO starjoin_var_free_d%s SELECT generate_series(1, 10)', i);
    EXECUTE format('ANALYZE starjoin_var_free_d%s', i);
  END LOOP;
END;
$$;

CREATE TABLE starjoin_var_free_f (id int PRIMARY KEY,
  d1 int NOT NULL REFERENCES starjoin_var_free_d1,
  d2 int NOT NULL REFERENCES starjoin_var_free_d2,
  d3 int NOT NULL REFERENCES starjoin_var_free_d3,
  d4 int NOT NULL REFERENCES starjoin_var_free_d4,
  d5 int NOT NULL REFERENCES starjoin_var_free_d5,
  d6 int NOT NULL REFERENCES starjoin_var_free_d6,
  d7 int NOT NULL REFERENCES starjoin_var_free_d7,
  d8 int NOT NULL REFERENCES starjoin_var_free_d8,
  d9 int NOT NULL REFERENCES starjoin_var_free_d9,
  d10 int NOT NULL REFERENCES starjoin_var_free_d10,
  d11 int NOT NULL REFERENCES starjoin_var_free_d11)
  WITH (autovacuum_enabled = off);

INSERT INTO starjoin_var_free_f
SELECT i, d, d, d, d, d, d, d, d, d, d, d
  FROM (SELECT i, 1 + i % 10 AS d FROM generate_series(1, 100) i) s;

ANALYZE starjoin_var_free_f;

--
-- With join_collapse_limit = 20 the star with 11 dimensions (12 rels) is a
-- single subproblem, reaching the default geqo_threshold = 12.  But the
-- dimensions count as only 4 rels, so without a Var-free qual the star is
-- planned by the standard join search.
--
SET join_collapse_limit = 20;

SELECT $$
SELECT count(*) FROM starjoin_var_free_f AS f
  JOIN starjoin_var_free_d1 AS d1 ON f.d1 = d1.id
  JOIN starjoin_var_free_d2 AS d2 ON f.d2 = d2.id
  JOIN starjoin_var_free_d3 AS d3 ON f.d3 = d3.id
  JOIN starjoin_var_free_d4 AS d4 ON f.d4 = d4.id
  JOIN starjoin_var_free_d5 AS d5 ON f.d5 = d5.id
  JOIN starjoin_var_free_d6 AS d6 ON f.d6 = d6.id
  JOIN starjoin_var_free_d7 AS d7 ON f.d7 = d7.id
  JOIN starjoin_var_free_d8 AS d8 ON f.d8 = d8.id
  JOIN starjoin_var_free_d9 AS d9 ON f.d9 = d9.id
  JOIN starjoin_var_free_d10 AS d10 ON f.d10 = d10.id
  JOIN starjoin_var_free_d11 AS d11 ON f.d11 = d11.id
$$ AS q_star \gset

-- with the GUC disabled, GEQO is used as usual
SELECT starjoin_var_free_geqo_used(:'q_star', false) AS star_geqo_off;
-- the dimensions count as fewer rels toward geqo_threshold
SELECT NOT starjoin_var_free_geqo_used(:'q_star', true) AS star_standard_search;

-- XXX a pseudoconstant qual disables the starjoin clusters
SELECT NOT starjoin_var_free_geqo_used(:'q_star' || $$WHERE now() > '2000-01-01'$$, true)
       AS where_now_standard_search;

-- XXX the same for an uncorrelated sub-SELECT (an InitPlan)
SELECT NOT starjoin_var_free_geqo_used(:'q_star' ||
         'WHERE EXISTS (SELECT 1 FROM starjoin_var_free_d1 WHERE id = 0)', true)
       AS where_exists_standard_search;

-- XXX the same for a qual in the ON clause of one dimension's join, which
-- excludes all the dimensions, not just that one
SELECT NOT starjoin_var_free_geqo_used(replace(:'q_star', 'f.d11 = d11.id',
         $$f.d11 = d11.id AND now() > '2000-01-01'$$), true)
       AS join_on_standard_search;

RESET join_collapse_limit;

DROP FUNCTION starjoin_var_free_geqo_used(text, bool);

DROP TABLE starjoin_var_free_f, starjoin_var_free_d1, starjoin_var_free_d2,
  starjoin_var_free_d3, starjoin_var_free_d4, starjoin_var_free_d5,
  starjoin_var_free_d6, starjoin_var_free_d7, starjoin_var_free_d8,
  starjoin_var_free_d9, starjoin_var_free_d10, starjoin_var_free_d11;
