/* contrib/earthdistance/earthdistance--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION earthdistance UPDATE TO '1.2'" to load this file. \quit

DO LANGUAGE plpgsql
$$
DECLARE
	v_cubenspname TEXT;
BEGIN
	SELECT nspname INTO v_cubenspname
	FROM  pg_extension, pg_namespace
	WHERE pg_extension.extnamespace = pg_namespace.oid
	AND extname='cube';

	IF v_cubenspname != '@extschema@' THEN
		RAISE EXCEPTION 'earthdistance extension must be installed in the same schema as the cube extension';
	END IF;
END
$$;

ALTER FUNCTION earth() SET SEARCH_PATH=@extschema@;
ALTER FUNCTION sec_to_gc(float8) SET SEARCH_PATH=@extschema@;
ALTER FUNCTION gc_to_sec(float8) SET SEARCH_PATH=@extschema@;
ALTER FUNCTION ll_to_earth(float8, float8) SET SEARCH_PATH=@extschema@;
ALTER FUNCTION latitude(earth) SET SEARCH_PATH=@extschema@;
ALTER FUNCTION longitude(earth) SET SEARCH_PATH=@extschema@;
ALTER FUNCTION earth_distance(earth, earth) SET SEARCH_PATH=@extschema@;
ALTER FUNCTION earth_box(earth, float8) SET SEARCH_PATH=@extschema@;
ALTER FUNCTION geo_distance(point, point) SET SEARCH_PATH=@extschema@;
