/* src/test/modules/test_listsort/test_listsort--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_listsort" to load this file. \quit

CREATE FUNCTION test_listsort (arg1 integer, arg2 integer, arg3 integer, arg4 boolean)
RETURNS float[]
STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;