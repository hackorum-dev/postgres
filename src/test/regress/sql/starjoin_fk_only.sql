-- A satisfied FK references rows in the partitions, but JOIN ONLY
-- scans the empty parent. It must not count as a single-row lookup.
\set ON_ERROR_STOP on
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
CREATE TEMP TABLE partdim (k int PRIMARY KEY) PARTITION BY RANGE (k);
CREATE TEMP TABLE partdim_default PARTITION OF partdim DEFAULT;
INSERT INTO partdim VALUES (1);
CREATE TEMP TABLE fact (
  k int NOT NULL CONSTRAINT partdim_fk REFERENCES partdim,
  a int NOT NULL REFERENCES d1, b int NOT NULL REFERENCES d2,
  c int NOT NULL REFERENCES d3, d int NOT NULL REFERENCES d4
);
INSERT INTO fact SELECT 1, 1, 1, 1, 1 FROM generate_series(1, 100);

SELECT $$SELECT count(*) FROM fact f JOIN ONLY partdim p ON f.k = p.k
  JOIN ONLY d1 ON f.a = d1.id JOIN ONLY d2 ON f.b = d2.id
  JOIN ONLY d3 ON f.c = d3.id JOIN ONLY d4 ON f.d = d4.id$$ AS q_only \gset
SELECT replace(:'q_only', 'ONLY partdim', 'partdim') AS q_all \gset
-- 100 facts; 100 matches with the partition, none with ONLY.
SELECT (SELECT count(*) FROM fact) AS facts, (:q_all) AS all_matches, (:q_only) AS only_matches;

-- As in starjoin_fk_collation, the query lists the dimensions in canonical
-- order. If all qualify, the search needs about as much planner memory as
-- forcing that order; otherwise it builds additional joinrels.
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
SELECT pg_temp.dimensions_qualify(:'q_only') AS only_parent_qualifies;
SET LOCAL enable_starjoin_ordering = off;
SELECT pg_temp.dimensions_qualify(:'q_only') AS qualifies_with_feature_off;
SET LOCAL enable_starjoin_ordering = on;
ALTER TABLE fact DROP CONSTRAINT partdim_fk;
SELECT pg_temp.dimensions_qualify(:'q_only') AS qualifies_without_partdim_fk;

-- ONLY is fine for a directly referenced leaf partition or ordinary table.
ALTER TABLE fact ADD FOREIGN KEY (k) REFERENCES partdim_default;
SELECT replace(:'q_only', 'ONLY partdim p', 'ONLY partdim_default p') AS q_leaf \gset
SELECT (:q_leaf) AS leaf_matches, pg_temp.dimensions_qualify(:'q_leaf') AS leaf_qualifies;
ROLLBACK;
