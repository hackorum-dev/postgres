/* src/test/modules/test_tableam_logical/test_tableam_logical--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_tableam_logical" to load this file. \quit

CREATE FUNCTION test_tableam_decode_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION test_tableam_nodecode_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION test_tableam_overclaim_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION test_tableam_supports_features(regclass, int)
RETURNS int
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE ACCESS METHOD test_decode TYPE TABLE
  HANDLER test_tableam_decode_handler;
CREATE ACCESS METHOD test_nodecode TYPE TABLE
  HANDLER test_tableam_nodecode_handler;
CREATE ACCESS METHOD test_overclaim TYPE TABLE
  HANDLER test_tableam_overclaim_handler;
