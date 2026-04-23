/* contrib/pg_trgm/pg_trgm--1.6--1.7.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_trgm UPDATE TO '1.7'" to load this file. \quit

-- Create collation-aware comparison function for trigrams
CREATE FUNCTION gin_compare_value_trgm(int4, int4)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Replace btint4cmp with gin_compare_value_trgm in the operator family
-- This ensures trigram comparisons respect collation settings
-- First drop the old function, then add the new one
ALTER OPERATOR FAMILY gin_trgm_ops USING gin DROP
        FUNCTION        1 (text, text);

ALTER OPERATOR FAMILY gin_trgm_ops USING gin ADD
        FUNCTION        1 (text, text) gin_compare_value_trgm (int4, int4);

