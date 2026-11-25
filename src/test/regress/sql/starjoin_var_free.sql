--
-- Checks for star queries with Var-free quals and enable_starjoin_ordering
--
-- A qual marked pseudoconstant is evaluated as a one-time qual in a gating
-- Result node.  Although its required_relids can cover the whole join domain,
-- it is not a join condition linking those relations.  Ignore it for both
-- starjoin eligibility and join enumeration, without changing where it is
-- evaluated.  Such quals must neither disable clusters nor cause excessive
-- planning memory use.
--
-- EXPLAIN does not show whether a query was planned by GEQO, so the search
-- selection checks detect GEQO by making it fail (see
-- starjoin_var_free_geqo_used).
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

-- memory used by the planning (in kB)
CREATE FUNCTION starjoin_var_free_planning_memory(query text) RETURNS bigint
LANGUAGE plpgsql SET enable_starjoin_ordering = on AS $$
DECLARE
  plan json;
BEGIN
  EXECUTE 'EXPLAIN (FORMAT JSON, MEMORY, COSTS OFF) ' || query INTO plan;
  RETURN (plan->0->'Planning'->>'Memory Used')::bigint;
END;
$$;

-- plan of the join tree of an aggregate query, below the gating Result node
-- of a pseudoconstant qual (with enable_starjoin_ordering enabled/disabled)
CREATE FUNCTION starjoin_var_free_join_tree(query text, starjoin bool) RETURNS jsonb
LANGUAGE plpgsql AS $$
DECLARE
  plan jsonb;
BEGIN
  PERFORM set_config('enable_starjoin_ordering', starjoin::text, true);
  EXECUTE 'EXPLAIN (FORMAT JSON, COSTS OFF) ' || query INTO plan;
  plan := plan #> '{0,Plan,Plans,0}';
  IF plan->>'Node Type' = 'Result' THEN
    plan := plan #> '{Plans,0}';
  END IF;
  RETURN plan;
END;
$$;

-- Execute with both search strategies for the eleven-dimension star.
CREATE FUNCTION starjoin_var_free_check(query text,
                                        OUT starjoin_on text,
                                        OUT starjoin_off text)
LANGUAGE plpgsql AS $$
BEGIN
  PERFORM set_config('enable_starjoin_ordering', 'on', true);
  EXECUTE format('SELECT row(q.*)::text FROM (%s) q', query) INTO starjoin_on;
  PERFORM set_config('enable_starjoin_ordering', 'off', true);
  EXECUTE format('SELECT row(q.*)::text FROM (%s) q', query) INTO starjoin_off;
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
-- dimensions count as only 4 rels, so even with a pseudoconstant qual the
-- star is planned by the standard join search.
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

-- A stable Var-free WHERE qual does not disable the starjoin clusters.
SELECT NOT starjoin_var_free_geqo_used(:'q_star' || $$WHERE now() > '2000-01-01'$$, true)
       AS where_now_standard_search;

-- The same applies to an uncorrelated sub-SELECT (an InitPlan).
SELECT NOT starjoin_var_free_geqo_used(:'q_star' ||
         'WHERE EXISTS (SELECT 1 FROM starjoin_var_free_d1 WHERE id = 0)', true)
       AS where_exists_standard_search;

-- A pseudoconstant in an inner-join ON clause is also a gating qual.
SELECT NOT starjoin_var_free_geqo_used(replace(:'q_star', 'f.d11 = d11.id',
         $$f.d11 = d11.id AND now() > '2000-01-01'$$), true)
       AS join_on_standard_search;

-- the qual does not make the standard join search consider all subsets of
-- the dimensions
SELECT starjoin_var_free_planning_memory(:'q_star' || $$WHERE now() > '2000-01-01'$$)
       < 2 * starjoin_var_free_planning_memory(:'q_star')
       AS where_now_planning_memory;

-- Nor does it make rels without a join clause between them joinable, even
-- without starjoin clusters (there are no foreign key joins in this chain),
-- as the search would then join all subsets of the rels.  The join clauses
-- are inequalities, so that they don't link all the rels through a single
-- equivalence class.
SELECT $$
SELECT count(*) FROM starjoin_var_free_d1 AS c1
  JOIN starjoin_var_free_d2 AS c2 ON c1.id < c2.id
  JOIN starjoin_var_free_d3 AS c3 ON c2.id < c3.id
  JOIN starjoin_var_free_d4 AS c4 ON c3.id < c4.id
  JOIN starjoin_var_free_d5 AS c5 ON c4.id < c5.id
  JOIN starjoin_var_free_d6 AS c6 ON c5.id < c6.id
  JOIN starjoin_var_free_d7 AS c7 ON c6.id < c7.id
  JOIN starjoin_var_free_d8 AS c8 ON c7.id < c8.id
$$ AS q_chain \gset

SELECT starjoin_var_free_planning_memory(:'q_chain' || $$WHERE now() > '2000-01-01'$$)
       < 2 * starjoin_var_free_planning_memory(:'q_chain')
       AS chain_where_now_planning_memory;

-- Nor does the qual make rels without join clauses look like they have some,
-- which would leave their Cartesian products to the last-ditch joins at the
-- top of the join tree.  The plan is the same as without the qual, with
-- starjoin ordering enabled or disabled.
SELECT $$
SELECT count(*) FROM starjoin_var_free_f AS f
  JOIN starjoin_var_free_d1 AS d1 ON f.d1 = d1.id
  JOIN starjoin_var_free_d2 AS d2 ON f.d2 = d2.id
  JOIN starjoin_var_free_d3 AS d3 ON f.d3 = d3.id
  CROSS JOIN starjoin_var_free_d4 AS x1
  CROSS JOIN starjoin_var_free_d5 AS x2
 WHERE x1.id <= 2 AND x2.id = 1
$$ AS q_cross \gset

SELECT starjoin_var_free_join_tree(:'q_cross', false) =
       starjoin_var_free_join_tree(:'q_cross' || $$AND now() > '2000-01-01'$$, false)
       AS cross_where_now_same_plan_off,
       starjoin_var_free_join_tree(:'q_cross', true) =
       starjoin_var_free_join_tree(:'q_cross' || $$AND now() > '2000-01-01'$$, true)
       AS cross_where_now_same_plan_on;

-- A volatile Var-free qual is not pseudoconstant and still excludes dimensions.
SELECT starjoin_var_free_geqo_used(:'q_star' || 'WHERE random() < 0.5', true)
       AS where_volatile_geqo;

-- Execution-time TRUE, FALSE and NULL gates must retain their filtering effect,
-- with standard search (starjoin on) and GEQO (starjoin off).
SELECT * FROM starjoin_var_free_check(:'q_star' ||
         $$WHERE now() > '-infinity'::timestamptz$$);
SELECT * FROM starjoin_var_free_check(:'q_star' ||
         'WHERE EXISTS (SELECT 1 FROM starjoin_var_free_d1 WHERE id = 0)');
SELECT * FROM starjoin_var_free_check(:'q_star' ||
         'WHERE (SELECT bool_or(id > 0) FROM starjoin_var_free_d1 WHERE id = 0)');

-- A Var-free outer-join ON qual must not become a gate above the outer join.
SELECT * FROM starjoin_var_free_check($$
SELECT count(*), count(d1.id)
  FROM starjoin_var_free_f AS f
  LEFT JOIN (starjoin_var_free_d1 AS d1 CROSS JOIN starjoin_var_free_d2 AS d2)
    ON f.d1 = d1.id AND f.d2 = d2.id AND now() < '-infinity'::timestamptz
$$);

-- A fact-free subproblem still needs the Cartesian-product fallback.
SET join_collapse_limit = 1;
SELECT * FROM starjoin_var_free_check($$
SELECT count(*)
  FROM starjoin_var_free_f AS f
  JOIN (starjoin_var_free_d1 AS d1 CROSS JOIN starjoin_var_free_d2 AS d2)
    ON f.d1 = d1.id AND f.d2 = d2.id
 WHERE now() > '-infinity'::timestamptz
$$);

RESET join_collapse_limit;

DROP FUNCTION starjoin_var_free_geqo_used(text, bool);
DROP FUNCTION starjoin_var_free_planning_memory(text);
DROP FUNCTION starjoin_var_free_join_tree(text, bool);
DROP FUNCTION starjoin_var_free_check(text);

DROP TABLE starjoin_var_free_f, starjoin_var_free_d1, starjoin_var_free_d2,
  starjoin_var_free_d3, starjoin_var_free_d4, starjoin_var_free_d5,
  starjoin_var_free_d6, starjoin_var_free_d7, starjoin_var_free_d8,
  starjoin_var_free_d9, starjoin_var_free_d10, starjoin_var_free_d11;
