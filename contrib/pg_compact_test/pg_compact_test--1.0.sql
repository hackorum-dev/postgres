/* contrib/pg_compact_test/pg_compact_test--1.0.sql */

\echo Use "CREATE EXTENSION pg_compact_test" to load this file. \quit

CREATE FUNCTION pg_test_compact_buffer(rel regclass,
                                       target_block bigint,
                                       source_block bigint,
                                       tuple_size integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_test_compact_buffer'
LANGUAGE C STRICT;

CREATE FUNCTION pg_test_relocate_tuple(rel regclass,
                                       source_tid tid,
                                       target_block bigint)
RETURNS tid
AS 'MODULE_PATHNAME', 'pg_test_relocate_tuple'
LANGUAGE C STRICT;
