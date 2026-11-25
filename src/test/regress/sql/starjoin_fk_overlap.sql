--
-- Overlapping foreign keys between a starjoin fact and dimension
--
BEGIN;
SET LOCAL enable_starjoin_ordering = on;

CREATE TEMP TABLE d1 (id int PRIMARY KEY);
CREATE TEMP TABLE d2 (id int PRIMARY KEY);
CREATE TEMP TABLE d3 (id int PRIMARY KEY);
CREATE TEMP TABLE d4 (id int PRIMARY KEY);
CREATE TEMP TABLE pair (k int PRIMARY KEY, v int UNIQUE NOT NULL, UNIQUE (k, v));
CREATE TEMP TABLE fact (
  a int NOT NULL REFERENCES d1, b int NOT NULL REFERENCES d2,
  c int NOT NULL REFERENCES d3, d int NOT NULL REFERENCES d4,
  k int NOT NULL, v int NOT NULL,
  CONSTRAINT a_short FOREIGN KEY (k) REFERENCES pair (k),
  CONSTRAINT m_full FOREIGN KEY (k, v) REFERENCES pair (k, v)
);

SELECT $$SELECT count(*) FROM fact f JOIN pair p ON f.k = p.k AND f.v = p.v
  JOIN d1 ON f.a = d1.id JOIN d2 ON f.b = d2.id
  JOIN d3 ON f.c = d3.id JOIN d4 ON f.d = d4.id$$ AS query \gset

-- If all dimensions qualify, the join search builds only the join rels of
-- the canonical order, which is the order the query lists them in.  It then
-- needs about as much planner memory as with join_collapse_limit = 1, which
-- forces that order.  A dimension that does not qualify can also be joined
-- at any other point, so the search takes noticeably more memory.  The four
-- dimensions d1 to d4 always qualify and widen the difference.
CREATE FUNCTION pg_temp.dimensions_qualify(query text) RETURNS bool
LANGUAGE plpgsql SET join_collapse_limit = 8 AS $$
DECLARE
  plan json;
  search_memory bigint;
BEGIN
  -- the first planning after DDL also fills the caches
  EXECUTE 'EXPLAIN ' || query;
  EXECUTE 'EXPLAIN (MEMORY, FORMAT JSON) ' || query INTO plan;
  search_memory := plan->0->'Planning'->>'Memory Used';
  SET LOCAL join_collapse_limit = 1;
  EXECUTE 'EXPLAIN (MEMORY, FORMAT JSON) ' || query INTO plan;
  RETURN search_memory <= (plan->0->'Planning'->>'Memory Used')::bigint * 1.1;
END;
$$;

-- The composite FK accounts for both equalities, regardless of which FK
-- is examined first.  Vary both creation and name order.
SELECT pg_temp.dimensions_qualify(:'query') AS short_first_qualifies;
ALTER TABLE fact DROP CONSTRAINT a_short;
SELECT pg_temp.dimensions_qualify(:'query') AS full_only_qualifies;
ALTER TABLE fact ADD CONSTRAINT z_short FOREIGN KEY (k) REFERENCES pair (k);
SELECT pg_temp.dimensions_qualify(:'query') AS full_first_qualifies;

-- The short FK alone is insufficient.  Nor can two independent FKs
-- prove that both equalities match the same dimension row.
ALTER TABLE fact DROP CONSTRAINT m_full;
SELECT NOT pg_temp.dimensions_qualify(:'query') AS short_only_does_not_qualify;
ALTER TABLE fact ADD FOREIGN KEY (v) REFERENCES pair (v);
SELECT NOT pg_temp.dimensions_qualify(:'query') AS separate_keys_do_not_qualify;
ROLLBACK;
