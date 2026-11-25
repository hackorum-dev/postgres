# Standalone reproducer: isolationtester "dbname=scratch" < brin-hot-read-committed.spec
# The first READ COMMITTED probe should count (1, 1).  On an affected server
# the BRIN scan misses the old row and the probe counts (0, 1).  The next
# statement sees the committed update and should count (0, 0) on either server.

setup
{
    CREATE TABLE brin_hot_rc (k int)
      WITH (fillfactor = 50, autovacuum_enabled = off);
    INSERT INTO brin_hot_rc VALUES (1);
    CREATE INDEX brin_hot_rc_idx ON brin_hot_rc USING brin (k)
      WITH (pages_per_range = 1);
}

teardown { DROP TABLE brin_hot_rc; }

session reader
setup
{
    BEGIN ISOLATION LEVEL READ COMMITTED;
    SET LOCAL enable_seqscan = off;
    SET LOCAL enable_bitmapscan = on;
}

# Both scans need the key from the CTE, so neither can start before the
# writer releases the lock.  The aggregate FILTER leaves the heap scan
# without an index condition.
step plan_probe
{
    PREPARE count_ones AS
      WITH probe_key AS MATERIALIZED (
        SELECT 1 AS k FROM pg_advisory_xact_lock('brin_hot_rc'::regclass::int8)
      )
      SELECT (SELECT count(*) FROM brin_hot_rc t WHERE t.k = p.k) AS brin_ones,
             (SELECT count(*) FILTER (WHERE t.k = p.k)
                FROM brin_hot_rc t) AS heap_ones
        FROM probe_key p;
    EXPLAIN (COSTS OFF) EXECUTE count_ones;
}
step probe { EXECUTE count_ones; }
step finish { COMMIT; }

session writer
setup { SET max_parallel_maintenance_workers = 0; }

# A session lock survives the update's commit and VACUUM.  Unlike REINDEX,
# VACUUM can resummarize the range while the reader has the index open.
step hold_probe { SELECT pg_advisory_lock('brin_hot_rc'::regclass::int8); }
step change_key
{
    BEGIN;
    UPDATE brin_hot_rc SET k = 2;
    SELECT pg_stat_get_xact_tuples_hot_updated('brin_hot_rc'::regclass)
      AS hot_updates;
    COMMIT;
}
step desummarize { SELECT brin_desummarize_range('brin_hot_rc_idx', 0); }
step vacuum_range { VACUUM (INDEX_CLEANUP ON) brin_hot_rc; }
step release_probe { SELECT pg_advisory_unlock('brin_hot_rc'::regclass::int8); }

permutation plan_probe hold_probe probe change_key desummarize vacuum_range
  release_probe probe finish
