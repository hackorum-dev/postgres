/* src/test/modules/test_subxact_commit/test_subxact_commit--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_subxact_commit" to load this file. \quit

-- Forces the shared library to load so _PG_init can register the callback
-- and define test_subxact_commit.force_error.
CREATE FUNCTION test_subxact_commit_init()
	RETURNS void
	AS 'MODULE_PATHNAME', 'test_subxact_commit_init'
	LANGUAGE C;
