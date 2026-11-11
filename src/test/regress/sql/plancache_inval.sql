-- Check plan invalidation when catalog definitions change.
-- Replacing a cast must invalidate queries using its old function.  Create
-- both functions first, so only the cast changes after preparing the queries.
CREATE TYPE prepare_cast_src AS ENUM ('x');
CREATE TYPE prepare_cast_dst AS ENUM ('v1', 'v2');
CREATE FUNCTION prepare_cast_v1(prepare_cast_src) RETURNS prepare_cast_dst
  LANGUAGE SQL IMMUTABLE AS $$ SELECT 'v1'::prepare_cast_dst $$;
CREATE FUNCTION prepare_cast_v2(prepare_cast_src) RETURNS prepare_cast_dst
  LANGUAGE SQL IMMUTABLE AS $$ SELECT 'v2'::prepare_cast_dst $$;
CREATE CAST (prepare_cast_src AS prepare_cast_dst)
  WITH FUNCTION prepare_cast_v1(prepare_cast_src);

SET plan_cache_mode = force_generic_plan;
PREPARE cast_generic(prepare_cast_src) AS SELECT $1::prepare_cast_dst AS value;
EXECUTE cast_generic('x');
SET plan_cache_mode = force_custom_plan;
PREPARE cast_custom(prepare_cast_src) AS SELECT $1::prepare_cast_dst AS value;
EXECUTE cast_custom('x');

DROP CAST (prepare_cast_src AS prepare_cast_dst);
CREATE CAST (prepare_cast_src AS prepare_cast_dst)
  WITH FUNCTION prepare_cast_v2(prepare_cast_src);

-- Re-analyze the queries for both generic and custom plans.
SET plan_cache_mode = force_generic_plan;
EXECUTE cast_generic('x');
SET plan_cache_mode = force_custom_plan;
EXECUTE cast_custom('x');

-- Fresh analysis must agree with the revalidated prepared statements.
SELECT ('x'::prepare_cast_src)::prepare_cast_dst AS value;
PREPARE cast_fresh(prepare_cast_src) AS SELECT $1::prepare_cast_dst AS value;
EXECUTE cast_fresh('x');

DEALLOCATE cast_generic;
DEALLOCATE cast_custom;
DEALLOCATE cast_fresh;
RESET plan_cache_mode;
DROP CAST (prepare_cast_src AS prepare_cast_dst);
DROP FUNCTION prepare_cast_v1(prepare_cast_src);
DROP FUNCTION prepare_cast_v2(prepare_cast_src);
DROP TYPE prepare_cast_src;
DROP TYPE prepare_cast_dst;

-- Replacing an operator-family support function must invalidate analyzed
-- queries as well as generic plans.  RANGE frame offsets retain the selected
-- in_range function OID in their WindowClause.  Create both functions before
-- preparing the queries, so pg_proc invalidations cannot mask the dependency.
CREATE SCHEMA amproc_plan;
SET search_path = amproc_plan, pg_catalog;
CREATE FUNCTION range1(int, int, int, boolean, boolean) RETURNS boolean
  LANGUAGE sql IMMUTABLE STRICT
  RETURN pg_catalog.in_range($1, $2, $3, $4, $5);
CREATE FUNCTION range2(int, int, int, boolean, boolean) RETURNS boolean
  LANGUAGE sql IMMUTABLE STRICT
  RETURN pg_catalog.in_range($1, $2, 2 * $3, $4, $5);
CREATE OPERATOR <^ (LEFTARG = int, RIGHTARG = int, FUNCTION = int4lt);
CREATE OPERATOR >^ (LEFTARG = int, RIGHTARG = int, FUNCTION = int4gt);
CREATE OPERATOR FAMILY range_ops USING btree;
ALTER OPERATOR FAMILY range_ops USING btree ADD
  OPERATOR 1 <^ (int, int),
  OPERATOR 2 <= (int, int),
  OPERATOR 3 = (int, int),
  OPERATOR 4 >= (int, int),
  OPERATOR 5 >^ (int, int),
  FUNCTION 1 (int, int) btint4cmp(int, int),
  FUNCTION 3 (int, int) range1(int, int, int, boolean, boolean);

PREPARE amproc_generic(int) AS
SELECT array_agg(preceding ORDER BY x) AS preceding,
       array_agg(following ORDER BY x) AS following
FROM (SELECT x,
             sum(x) OVER (ORDER BY x USING <^
                          RANGE BETWEEN $1 PRECEDING AND CURRENT ROW) AS preceding,
             sum(x) OVER (ORDER BY x USING <^
                          RANGE BETWEEN CURRENT ROW AND $1 FOLLOWING) AS following
      FROM (VALUES (1), (2), (3), (4)) AS v(x)) AS s;
PREPARE amproc_custom(int) AS
SELECT array_agg(preceding ORDER BY x) AS preceding,
       array_agg(following ORDER BY x) AS following
FROM (SELECT x,
             sum(x) OVER (ORDER BY x USING <^
                          RANGE BETWEEN $1 PRECEDING AND CURRENT ROW) AS preceding,
             sum(x) OVER (ORDER BY x USING <^
                          RANGE BETWEEN CURRENT ROW AND $1 FOLLOWING) AS following
      FROM (VALUES (1), (2), (3), (4)) AS v(x)) AS s;
SET plan_cache_mode = force_generic_plan;
EXECUTE amproc_generic(1);
SET plan_cache_mode = force_custom_plan;
EXECUTE amproc_custom(1);

BEGIN;
ALTER OPERATOR FAMILY range_ops USING btree DROP FUNCTION 3 (int, int);
ALTER OPERATOR FAMILY range_ops USING btree ADD
  FUNCTION 3 (int, int) range2(int, int, int, boolean, boolean);
COMMIT;

-- Both old prepared queries should agree with a newly analyzed query.
SET plan_cache_mode = force_generic_plan;
EXECUTE amproc_generic(1);
SET plan_cache_mode = force_custom_plan;
EXECUTE amproc_custom(1);
SELECT array_agg(preceding ORDER BY x) AS preceding,
       array_agg(following ORDER BY x) AS following
FROM (SELECT x,
             sum(x) OVER (ORDER BY x USING <^
                          RANGE BETWEEN 1 PRECEDING AND CURRENT ROW) AS preceding,
             sum(x) OVER (ORDER BY x USING <^
                          RANGE BETWEEN CURRENT ROW AND 1 FOLLOWING) AS following
      FROM (VALUES (1), (2), (3), (4)) AS v(x)) AS s;

DEALLOCATE amproc_generic;
DEALLOCATE amproc_custom;
RESET plan_cache_mode;
DROP OPERATOR FAMILY range_ops USING btree;
DROP OPERATOR <^ (int, int);
DROP OPERATOR >^ (int, int);
DROP FUNCTION range1(int, int, int, boolean, boolean);
DROP FUNCTION range2(int, int, int, boolean, boolean);
DROP SCHEMA amproc_plan;
RESET search_path;
-- Check that collation DDL invalidates cached plans.
CREATE COLLATION public.pcachecoll FROM pg_catalog."C";
PREPARE pcachecollstmt AS
  SELECT pg_collation_for('a' COLLATE public.pcachecoll) = 'pcachecoll' AS matches;
EXECUTE pcachecollstmt;
ALTER COLLATION public.pcachecoll RENAME TO pcachecoll_old;
CREATE COLLATION public.pcachecoll FROM pg_catalog."C";
EXECUTE pcachecollstmt;
SELECT pg_collation_for('a' COLLATE public.pcachecoll) = 'pcachecoll' AS matches;
DEALLOCATE pcachecollstmt;
DROP COLLATION public.pcachecoll;
DROP COLLATION public.pcachecoll_old;
-- Recompute constant text search results when configuration mappings change.
CREATE TEXT SEARCH CONFIGURATION regress_ts_plan_config
  (COPY = pg_catalog.english);
PREPARE regress_ts_plan_query AS
  SELECT to_tsvector('regress_ts_plan_config'::regconfig, 'running') AS value;
EXECUTE regress_ts_plan_query;
ALTER TEXT SEARCH CONFIGURATION regress_ts_plan_config
  ALTER MAPPING FOR asciiword WITH pg_catalog.simple;
EXECUTE regress_ts_plan_query;
SELECT to_tsvector('regress_ts_plan_config'::regconfig, 'running') AS value;
DEALLOCATE regress_ts_plan_query;
DROP TEXT SEARCH CONFIGURATION regress_ts_plan_config;

-- Changing dictionary options also invalidates results folded into plans.
CREATE TEXT SEARCH DICTIONARY regress_ts_plan_dict
  (TEMPLATE = pg_catalog.simple, ACCEPT = false);
CREATE TEXT SEARCH CONFIGURATION regress_ts_plan_config
  (COPY = pg_catalog.english);
ALTER TEXT SEARCH CONFIGURATION regress_ts_plan_config
  ALTER MAPPING FOR asciiword WITH regress_ts_plan_dict, pg_catalog.english_stem;
PREPARE regress_ts_plan_query AS
  SELECT to_tsvector('regress_ts_plan_config'::regconfig, 'running') AS value;
EXECUTE regress_ts_plan_query;
ALTER TEXT SEARCH DICTIONARY regress_ts_plan_dict (ACCEPT = true);
EXECUTE regress_ts_plan_query;
SELECT to_tsvector('regress_ts_plan_config'::regconfig, 'running') AS value;
DEALLOCATE regress_ts_plan_query;
DROP TEXT SEARCH CONFIGURATION regress_ts_plan_config;
DROP TEXT SEARCH DICTIONARY regress_ts_plan_dict;

-- Dropping a configuration without mappings must invalidate plans too.
CREATE TEXT SEARCH CONFIGURATION regress_ts_plan_empty
  (PARSER = pg_catalog.default);
PREPARE regress_ts_plan_query AS
  SELECT to_tsvector('regress_ts_plan_empty'::regconfig, 'running') AS value;
EXECUTE regress_ts_plan_query;
DROP TEXT SEARCH CONFIGURATION regress_ts_plan_empty;
EXECUTE regress_ts_plan_query;
DEALLOCATE regress_ts_plan_query;
