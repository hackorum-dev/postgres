/* src/test/modules/test_btree_merge/test_btree_merge--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_btree_merge" to load this file. \quit

-- Test merge scan with integer columns
CREATE FUNCTION test_btree_merge_scan_int(
    table_name text,
    index_name text,
    prefix_values int4[],
    suffix_start int4,
    limit_count int4
) RETURNS TABLE (
    tuples_returned int4,
    tuples_accessed int4,
    maximum_required_fetches int4
) AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Fetch actual rows from merge scan (for correctness verification)
CREATE FUNCTION test_btree_merge_fetch_int(
    table_name text,
    index_name text,
    prefix_values int4[],
    suffix_start int4,
    limit_count int4
) RETURNS TABLE (
    prefix_col int4,
    suffix_col int4
) AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Test merge scan with timestamp suffix
CREATE FUNCTION test_btree_merge_scan_ts(
    table_name text,
    index_name text,
    prefix_values int4[],
    suffix_start timestamp,
    limit_count int4
) RETURNS TABLE (
    tuples_returned int4,
    tuples_accessed int4,
    maximum_required_fetches int4
) AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

