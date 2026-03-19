/* src/test/modules/test_dshash/test_dshash--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_dshash" to load this file. \quit

CREATE FUNCTION test_dshash_basic() RETURNS VOID
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_dshash_find_or_insert_oom_error() RETURNS VOID
	AS 'MODULE_PATHNAME' LANGUAGE C;
