/* src/test/modules/test_bloomfilter/test_lossless_bloomfilter--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_lossless_bloomfilter" to load this file. \quit

-- Test basic lossless bloom filter functionality
CREATE FUNCTION test_lossless_basic(nelements bigint, bloom_work_mem integer)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Test conditional hashing functionality
CREATE FUNCTION test_lossless_conditional(nelements bigint, bloom_work_mem integer)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Compare traditional vs lossless bloom filter performance
CREATE FUNCTION test_bloom_comparison(nelements bigint, bloom_work_mem integer)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Demonstrate VSBBF block decomposition
CREATE FUNCTION test_vsbbf_blocks(total_bits bigint)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT; 