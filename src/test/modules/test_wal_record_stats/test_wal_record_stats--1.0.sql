/* src/test/modules/test_wal_record_stats/test_wal_record_stats--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_wal_record_stats" to load this file. \quit

CREATE FUNCTION get_wal_record_stats_from_buffers(
    IN start_lsn pg_lsn,
    IN end_lsn pg_lsn,
    OUT resource_manager text,
    OUT record_type text,
    OUT count int8,
    OUT total_record_length int8,
    OUT total_main_data_length int8,
    OUT total_fpi_length int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'get_wal_record_stats_from_buffers'
LANGUAGE C STRICT PARALLEL SAFE;
