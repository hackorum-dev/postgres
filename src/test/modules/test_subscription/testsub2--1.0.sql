/* src/test/modules/test_subscription/testsub2--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION testsub2" to load this file. \quit

CREATE TYPE dummyint;

CREATE FUNCTION dummyint_in(cstring)
RETURNS dummyint
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION dummyint_out(dummyint)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE dummyint (
	   LIKE = pg_catalog.int4,
       INPUT = dummyint_in,
       OUTPUT = dummyint_out
);
