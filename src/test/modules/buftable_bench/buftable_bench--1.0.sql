/* src/test/modules/buftable_bench/buftable_bench--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION buftable_bench" to load this file. \quit

CREATE FUNCTION buftable_bench_probe(
	IN n int8,
	IN rounds int8 DEFAULT 1,
	IN random bool DEFAULT true,
	OUT op text,
	OUT avg_ns float8,
	OUT batch_size int8,
	OUT id int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'buftable_bench_probe'
LANGUAGE C;

REVOKE ALL ON FUNCTION buftable_bench_probe(int8, int8, bool) FROM PUBLIC;
