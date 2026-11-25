# Both scans use the same snapshot and should find the one row with k = 1.

setup
{
    CREATE TABLE brin_hot_rebuild (k int)
      WITH (fillfactor = 50, autovacuum_enabled = off);
    INSERT INTO brin_hot_rebuild VALUES (1);
    CREATE INDEX brin_hot_rebuild_idx ON brin_hot_rebuild USING brin (k)
      WITH (pages_per_range = 1);
}

teardown { DROP TABLE brin_hot_rebuild; }

session reader
setup
{
    BEGIN ISOLATION LEVEL REPEATABLE READ;
    SET LOCAL enable_seqscan = off;
    SET LOCAL enable_bitmapscan = on;
}

# Take the snapshot without locking the index: REINDEX needs an exclusive lock.
step pin_snapshot { SELECT pg_current_snapshot() IS NOT NULL AS snapshot_taken; }
step probe
{
    EXPLAIN (COSTS OFF) SELECT count(*) FROM brin_hot_rebuild WHERE k = 1;
    SELECT count(*) AS visible_ones FROM brin_hot_rebuild WHERE k = 1;
}
step use_heap { SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off; }
step finish { COMMIT; }

session writer
setup { SET max_parallel_maintenance_workers = 0; }
step change_key
{
    BEGIN;
    UPDATE brin_hot_rebuild SET k = 2;
    SELECT pg_stat_get_xact_tuples_hot_updated('brin_hot_rebuild'::regclass)
      AS hot_updates;
}
step commit_update { COMMIT; }
step rebuild { REINDEX INDEX brin_hot_rebuild_idx; }
step desummarize { SELECT brin_desummarize_range('brin_hot_rebuild_idx', 0); }
step vacuum_range { VACUUM (INDEX_CLEANUP ON) brin_hot_rebuild; }

permutation pin_snapshot change_key commit_update rebuild probe use_heap probe finish
permutation pin_snapshot change_key rebuild commit_update probe use_heap probe finish
permutation pin_snapshot change_key commit_update desummarize vacuum_range probe use_heap probe finish
