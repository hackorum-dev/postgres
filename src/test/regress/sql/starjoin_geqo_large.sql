--
-- STARJOIN_GEQO_LARGE
-- Benchmark an 80-dimension star with forced GEQO and with the default
-- geqo_threshold.  Mode names describe GUC settings, not the chosen search:
-- at threshold 2 both geqo_on modes use GEQO, without canonical pruning;
-- at threshold 12 the star counts as 8 rels when starjoin ordering is on,
-- and uses standard search instead.  Disabling GEQO provides a control.
-- Each dimension has the same size, so the resulting plan costs are equal.
--
-- For timing measurements, run this file in a scratch database with
-- psql -X -v ON_ERROR_STOP=1 -v starjoin_geqo_timings=1 -f <this file>.
-- Timings are optional because they are not stable regression-test output.
-- All objects and settings are rolled back.
--

BEGIN;

DO $$
DECLARE
  cols text := 'id int';
  exprs text := 'i';
  d int;
BEGIN
  FOR d IN 1..80 LOOP
    EXECUTE format('CREATE TABLE starjoin_geqo_d%s (id int PRIMARY KEY, v int)', d);
    EXECUTE format('INSERT INTO starjoin_geqo_d%s SELECT i, i FROM generate_series(1, 10) i', d);
    EXECUTE format('ANALYZE starjoin_geqo_d%s', d);
    cols := cols || format(', d%s int NOT NULL REFERENCES starjoin_geqo_d%s', d, d);
    exprs := exprs || ', 1 + i % 10';
  END LOOP;
  EXECUTE 'CREATE TABLE starjoin_geqo_f (' || cols || ')';
  EXECUTE 'INSERT INTO starjoin_geqo_f SELECT ' || exprs ||
          ' FROM generate_series(1, 100) i';
END;
$$;
ANALYZE starjoin_geqo_f;

SET LOCAL join_collapse_limit = 100;
SET LOCAL from_collapse_limit = 100;
SET LOCAL geqo = on;
SET LOCAL geqo_seed = 0.42;
SET LOCAL geqo_effort = 5;
SET LOCAL geqo_pool_size = 0;
SET LOCAL geqo_generations = 0;
SET LOCAL enable_eager_aggregate = off;
SET LOCAL max_parallel_workers_per_gather = 0;
SET LOCAL jit = off;

SELECT 'SELECT f.id FROM starjoin_geqo_f f ' ||
       string_agg(format('JOIN starjoin_geqo_d%s d%s ON f.d%s = d%s.id',
                         i, i, i, i), ' ' ORDER BY i) AS query
  FROM generate_series(1, 80) i \gset

-- Identify the chosen search outside the timing measurements.  GEQO cannot
-- allocate this population, while standard search ignores geqo_pool_size.
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

SET LOCAL geqo_threshold = 2;
SELECT starjoin_geqo_used(:'query', true)
       AND starjoin_geqo_used(:'query', false) AS forced_geqo;

-- At the default threshold, the 81 rels count as 8 with starjoin ordering
-- enabled, so only the disabled case uses GEQO.
SET LOCAL geqo_threshold = 12;
SELECT NOT starjoin_geqo_used(:'query', true)
       AND starjoin_geqo_used(:'query', false) AS default_search_ok;

CREATE FUNCTION starjoin_geqo_measure(query text)
RETURNS TABLE(geqo_threshold int, mode text, sample int,
              total_cost float8, planning_ms float8)
LANGUAGE plpgsql AS $$
DECLARE
  iteration int;
  plan json;
BEGIN
  FOREACH geqo_threshold IN ARRAY ARRAY[2, 12] LOOP
    PERFORM set_config('geqo_threshold', geqo_threshold::text, true);
    -- Warm all three modes at each threshold, then alternate the geqo_on
    -- variants across seven measurements.
    FOR iteration IN 0..7 LOOP
      FOREACH mode IN ARRAY
        CASE WHEN iteration % 2 = 0
             THEN ARRAY['geqo_on_starjoin_on', 'geqo_on_starjoin_off', 'geqo_off_starjoin_on']
             ELSE ARRAY['geqo_on_starjoin_off', 'geqo_on_starjoin_on', 'geqo_off_starjoin_on']
        END
      LOOP
        PERFORM set_config('enable_starjoin_ordering',
                           (mode <> 'geqo_on_starjoin_off')::text, true);
        PERFORM set_config('geqo', (mode <> 'geqo_off_starjoin_on')::text, true);
        EXECUTE 'EXPLAIN (SUMMARY ON, FORMAT JSON) ' || query INTO plan;
        IF iteration > 0 THEN
          sample := iteration;
          total_cost := (plan->0->'Plan'->>'Total Cost')::float8;
          planning_ms := (plan->0->>'Planning Time')::float8;
          RETURN NEXT;
        END IF;
      END LOOP;
    END LOOP;
  END LOOP;
END;
$$;

CREATE TEMP TABLE starjoin_geqo_times AS
  SELECT * FROM starjoin_geqo_measure(:'query');

SELECT geqo_threshold, min(total_cost) = max(total_cost) AS same_plan_cost
  FROM starjoin_geqo_times
 GROUP BY geqo_threshold
 ORDER BY geqo_threshold;

\if :{?starjoin_geqo_timings}
SELECT geqo_threshold, mode,
       round((percentile_cont(0.5) WITHIN GROUP (ORDER BY planning_ms))::numeric, 3)
         AS median_planning_ms,
       round(min(planning_ms)::numeric, 3) AS min_planning_ms,
       round(max(planning_ms)::numeric, 3) AS max_planning_ms,
       min(total_cost) AS total_cost
  FROM starjoin_geqo_times
 GROUP BY geqo_threshold, mode
 ORDER BY geqo_threshold, mode;
\endif

ROLLBACK;
