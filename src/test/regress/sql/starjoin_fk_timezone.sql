-- A time-zone-dependent FK comparison is not a guaranteed lookup.
BEGIN;
SET LOCAL geqo = off;
SET LOCAL enable_starjoin_ordering = on;
SET LOCAL TIME ZONE 'UTC';

CREATE TEMP TABLE d1 (id int PRIMARY KEY);
CREATE TEMP TABLE d2 (id int PRIMARY KEY);
CREATE TEMP TABLE d3 (id int PRIMARY KEY);
CREATE TEMP TABLE d4 (id int PRIMARY KEY);
INSERT INTO d1 VALUES (1);
INSERT INTO d2 VALUES (1);
INSERT INTO d3 VALUES (1);
INSERT INTO d4 VALUES (1);
CREATE TEMP TABLE local_times (tag int, k timestamp, PRIMARY KEY (tag, k));
CREATE TEMP TABLE instants (tag int, k timestamptz, PRIMARY KEY (tag, k));
INSERT INTO local_times VALUES (7, '2024-01-15 12:00');
INSERT INTO instants VALUES (7, '2024-01-15 12:00+00');
CREATE TEMP TABLE fact (
  tag smallint NOT NULL, k timestamptz NOT NULL,
  FOREIGN KEY (tag, k) REFERENCES local_times,
  FOREIGN KEY (tag, k) REFERENCES instants,
  a int NOT NULL REFERENCES d1, b int NOT NULL REFERENCES d2,
  c int NOT NULL REFERENCES d3, d int NOT NULL REFERENCES d4
);
INSERT INTO fact SELECT 7, '2024-01-15 12:00+00', 1, 1, 1, 1
  FROM generate_series(1, 100);
ANALYZE fact, local_times, instants, d1, d2, d3, d4;

SELECT $$SELECT count(*) FROM fact f JOIN local_times t ON f.tag = t.tag AND f.k = t.k
  JOIN d1 ON f.a = d1.id JOIN d2 ON f.b = d2.id
  JOIN d3 ON f.c = d3.id JOIN d4 ON f.d = d4.id$$ AS q_local \gset
SELECT replace(:'q_local', 'JOIN local_times', 'JOIN instants') AS q_instant \gset

-- Compare flat and forced join order, as in starjoin_fk_collation.
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
SELECT (:q_local) AS utc_matches,
       pg_temp.dimensions_qualify(:'q_local') AS utc_qualifies;
SET LOCAL TIME ZONE 'America/New_York';
SELECT (:q_local) AS ny_matches,
       pg_temp.dimensions_qualify(:'q_local') AS ny_qualifies;
-- The int/smallint comparison is cross-type but immutable. With two
-- timestamptz operands for the second column, this FK still qualifies.
SELECT (:q_instant) AS instant_matches,
       pg_temp.dimensions_qualify(:'q_instant') AS instant_qualifies;
ROLLBACK;
