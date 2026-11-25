--
-- A PERIOD foreign key guarantees coverage, not one matching row.
--
BEGIN;
SET LOCAL enable_starjoin_ordering = on;

CREATE TEMP TABLE d1 (id int PRIMARY KEY);
CREATE TEMP TABLE d2 (id int PRIMARY KEY);
CREATE TEMP TABLE d3 (id int PRIMARY KEY);
CREATE TEMP TABLE d4 (id int PRIMARY KEY);
INSERT INTO d1 VALUES (1);
INSERT INTO d2 VALUES (1);
INSERT INTO d3 VALUES (1);
INSERT INTO d4 VALUES (1);
CREATE TEMP TABLE periods (
  k int4range, r int4range UNIQUE, PRIMARY KEY (k, r WITHOUT OVERLAPS)
);
INSERT INTO periods
  SELECT '[1,2)'::int4range, int4range(i, i + 1) FROM generate_series(0, 7) i;
CREATE TEMP TABLE fact (
  a int NOT NULL REFERENCES d1, b int NOT NULL REFERENCES d2,
  c int NOT NULL REFERENCES d3, d int NOT NULL REFERENCES d4,
  k int4range NOT NULL, r int4range NOT NULL,
  CONSTRAINT coverage FOREIGN KEY (k, PERIOD r) REFERENCES periods,
  piece int4range NOT NULL REFERENCES periods (r)
);
INSERT INTO fact
  SELECT 1, 1, 1, 1, '[1,2)', '[0,8)', '[0,1)' FROM generate_series(1, 100);

SELECT $$SELECT count(*) FROM fact f JOIN periods p ON f.k = p.k AND f.r && p.r
  JOIN d1 ON f.a = d1.id JOIN d2 ON f.b = d2.id
  JOIN d3 ON f.c = d3.id JOIN d4 ON f.d = d4.id$$ AS query \gset
SELECT (SELECT count(*) FROM fact) AS fact_rows, (:query) AS joined_rows;

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

SELECT NOT pg_temp.dimensions_qualify(:'query') AS period_does_not_qualify;
-- An ordinary equality FK on the same pair of tables still qualifies.
SELECT pg_temp.dimensions_qualify(replace(:'query', 'f.k = p.k AND f.r && p.r',
                                          'f.piece = p.r')) AS ordinary_qualifies;
ALTER TABLE fact DROP CONSTRAINT coverage;
SELECT NOT pg_temp.dimensions_qualify(:'query') AS without_period_fk_does_not_qualify;
ROLLBACK;
