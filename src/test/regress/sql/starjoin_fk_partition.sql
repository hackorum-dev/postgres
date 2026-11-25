-- An FK to a partitioned dimension does not guarantee a match in one leaf.
BEGIN;
SET LOCAL geqo = off;
SET LOCAL enable_starjoin_ordering = on;

CREATE TEMP TABLE d1 (id int PRIMARY KEY);
CREATE TEMP TABLE d2 (id int PRIMARY KEY);
CREATE TEMP TABLE d3 (id int PRIMARY KEY);
CREATE TEMP TABLE d4 (id int PRIMARY KEY);
INSERT INTO d1 VALUES (1);
INSERT INTO d2 VALUES (1);
INSERT INTO d3 VALUES (1);
INSERT INTO d4 VALUES (1);
CREATE TEMP TABLE dim (k int PRIMARY KEY) PARTITION BY LIST (k);
CREATE TEMP TABLE dim_one PARTITION OF dim FOR VALUES IN (1);
CREATE TEMP TABLE dim_rest PARTITION OF dim DEFAULT;
INSERT INTO dim VALUES (1), (2);
CREATE TEMP TABLE fact (
  k int NOT NULL REFERENCES dim,
  a int NOT NULL REFERENCES d1, b int NOT NULL REFERENCES d2,
  c int NOT NULL REFERENCES d3, d int NOT NULL REFERENCES d4
);
INSERT INTO fact SELECT CASE WHEN i = 1 THEN 1 ELSE 2 END, 1, 1, 1, 1
  FROM generate_series(1, 100) i;
ANALYZE fact, dim_one, d1, d2, d3, d4;

SELECT $$SELECT count(*) FROM fact f JOIN d1 ON f.a = d1.id
  JOIN d2 ON f.b = d2.id JOIN d3 ON f.c = d3.id JOIN d4 ON f.d = d4.id
  JOIN dim_one p ON f.k = p.k$$ AS q_leaf \gset
SELECT replace(:'q_leaf', 'JOIN dim_one', 'JOIN dim') AS q_parent \gset

-- As in starjoin_fk_only, compare planner memory for flat and forced join
-- order. If the leaf qualifies, both searches build the canonical order.
CREATE FUNCTION pg_temp.dimensions_qualify(query text) RETURNS bool
LANGUAGE plpgsql SET join_collapse_limit = 8 AS $$
DECLARE
  plan json;
  search_memory bigint;
BEGIN
  EXECUTE 'EXPLAIN ' || query;
  EXECUTE 'EXPLAIN (MEMORY, FORMAT JSON) ' || query INTO plan;
  search_memory := plan->0->'Planning'->>'Memory Used';
  SET LOCAL join_collapse_limit = 1;
  EXECUTE 'EXPLAIN (MEMORY, FORMAT JSON) ' || query INTO plan;
  RETURN search_memory <= (plan->0->'Planning'->>'Memory Used')::bigint * 1.1;
END;
$$;
SELECT (:q_parent) AS parent_matches, (:q_leaf) AS leaf_matches,
       pg_temp.dimensions_qualify(:'q_leaf') AS parent_fk_qualifies;

-- A genuine FK to the same leaf suffices, even alongside the parent's FK.
UPDATE fact SET k = 1;
ALTER TABLE fact ADD FOREIGN KEY (k) REFERENCES dim_one;
SELECT (:q_leaf) AS leaf_matches,
       pg_temp.dimensions_qualify(:'q_leaf') AS direct_fk_qualifies;

-- Attach that FK to one on the referencing parent. It still guarantees a
-- match for this fact partition, unlike the referenced-side child entry.
CREATE TEMP TABLE fact_parent (
  LIKE fact, CONSTRAINT leaf_fk FOREIGN KEY (k) REFERENCES dim_one
) PARTITION BY LIST (k);
ALTER TABLE fact_parent ATTACH PARTITION fact DEFAULT;
SELECT pg_temp.dimensions_qualify(:'q_leaf') AS inherited_fk_qualifies;
ALTER TABLE fact_parent DROP CONSTRAINT leaf_fk;
SELECT pg_temp.dimensions_qualify(:'q_leaf') AS parent_fk_alone_qualifies;
ROLLBACK;
