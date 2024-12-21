/* contrib/stats_history/stats_history--1.0.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION stats_history UPDATE TO '1.0'" to load this file. \quit

/* create the function */
CREATE FUNCTION vacuum_history(
    OUT start_time timestamptz,
    OUT end_time timestamptz,
    OUT dbid oid,
    OUT relid regclass,
    OUT is_autovacuum bool,
    OUT is_aggressive bool,
    OUT is_wraparound bool,
    OUT index_cleanup bool,
    OUT pages_removed bigint,
    OUT pages_remain bigint,
    OUT pages_scanned bigint,
    OUT pages_frozen bigint,
    OUT pages_missed_dead bigint,
    OUT pages_new_visible bigint,
    OUT pages_new_frozen bigint,
    OUT pages_new_visible_frozen bigint,
    OUT tuples_removed bigint,
    OUT tuples_remain bigint,
    OUT tuples_not_removable bigint,
    OUT tuples_frozen bigint,
    OUT tuples_missed_dead bigint,
    OUT removable_cutoff int,
    OUT relfrozenxid_advance int,
    OUT relminmxid_advance int,
    OUT index_scans bigint,
    OUT index_count int,
    OUT index_pages bigint,
    OUT index_pages_newly_deleted bigint,
    OUT index_pages_deleted bigint,
    OUT index_pages_free bigint,
    OUT io_read_ms float,
    OUT io_write_ms float,
    OUT wal_records bigint,
    OUT wal_fpis bigint,
    OUT wal_bytes bigint,
    OUT cpu_user float,
    OUT cpu_system float,
    OUT cpu_elapsed float)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vacuum_history_1_0'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

CREATE VIEW vacuum_history AS
  SELECT * FROM vacuum_history();

GRANT SELECT ON vacuum_history TO PUBLIC;


CREATE FUNCTION checkpoint_history(
    OUT start_time timestamptz,
    OUT end_time timestamptz,
    OUT is_shutdown bool,
    OUT is_end_of_recovery bool,
    OUT is_immediate bool,
    OUT is_force bool,
    OUT is_wait bool,
    OUT is_wal bool,
    OUT is_time bool,
    OUT is_flush_all bool,
    OUT buffers_written int,
    OUT slru_written int,
    OUT segs_added int,
    OUT segs_removed int,
    OUT segs_recycled int,
    OUT write_ms bigint,
    OUT sync_ms bigint,
    OUT total_ms bigint,
    OUT sync_files int,
    OUT sync_longest_ms bigint,
    OUT sync_average_ms bigint,
    OUT distance_prev double precision,
    OUT distance_est double precision,
    OUT lsn pg_lsn,
    OUT redo_lsn pg_lsn)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'checkpoint_history_1_0'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

CREATE VIEW checkpoint_history AS
  SELECT * FROM checkpoint_history();

GRANT SELECT ON checkpoint_history TO PUBLIC;


CREATE FUNCTION stats_history_reset()
RETURNS void
AS 'MODULE_PATHNAME', 'stats_history_reset'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;
