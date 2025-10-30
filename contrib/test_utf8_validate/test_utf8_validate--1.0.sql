/* src/test/modules/test_lfind/test_lfind--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
-- \echo Use "CREATE EXTENSION test_utf8_validate_sources" to load this file. \quit

CREATE FUNCTION drive_utf8_validate(n int)
	RETURNS pg_catalog.void
	AS 'MODULE_PATHNAME' LANGUAGE C;