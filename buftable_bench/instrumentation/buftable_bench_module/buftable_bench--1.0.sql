/* src/test/modules/buftable_bench/buftable_bench--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION buftable_bench" to load this file. \quit

CREATE FUNCTION buftable_bench_probe(
	IN n int8,
	IN rounds int8 DEFAULT 1,
	IN random bool DEFAULT true,
	OUT op text,
	OUT avg_ns float8,
	OUT count int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'buftable_bench_probe'
LANGUAGE C;

REVOKE ALL ON FUNCTION buftable_bench_probe(int8, int8, bool) FROM PUBLIC;

-- Worst-case hash-collision probe: forces chains of length chain_len and times
-- lookup_hit_full + delete_drain (delete_drain == the flat table's extra
-- buffer-header spinlock hold in InvalidateBuffer).  See buftable_bench.c.
CREATE FUNCTION buftable_bench_collide(
	IN chain_len int8,
	IN total_entries int8 DEFAULT 0,
	IN rounds int8 DEFAULT 10,
	IN random bool DEFAULT true,
	IN delete_under_lock bool DEFAULT true,
	OUT op text,
	OUT avg_ns float8,
	OUT count int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'buftable_bench_collide'
LANGUAGE C;

REVOKE ALL ON FUNCTION buftable_bench_collide(int8, int8, int8, bool, bool) FROM PUBLIC;
