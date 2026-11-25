# Test index builds over HOT chains with older row versions still visible.
#
# An index with per-tuple entries uses the root TID and skips HOT-updated
# versions.  If an old snapshot might still see them, the build must set
# pg_index.indcheckxmin to keep it from using the index (see README.HOT).
# A summarizing index can include both old and new values.  s2 requests a
# parallel worker to exercise reporting of skipped versions to the leader.

setup
{
    -- No index and a low fillfactor: every UPDATE is a HOT update
    CREATE TABLE broken_hot (a int4, b int4[])
      WITH (autovacuum_enabled = off, fillfactor = 50, parallel_workers = 1);
    INSERT INTO broken_hot SELECT 1, ARRAY[1] FROM generate_series(1, 1000);
}

teardown
{
    DROP TABLE broken_hot;
}


session s1

# s1 uses an index whenever it is allowed to
setup
{
    SET enable_seqscan = off;
    BEGIN ISOLATION LEVEL REPEATABLE READ;
}

# s1's snapshot sees 1000 rows with the old values and none with the new
# values, before and after s2 builds its index
step s1_a_old { SELECT count(*) FROM broken_hot WHERE a = 1; }
step s1_a_new { SELECT count(*) FROM broken_hot WHERE a = 2; }
step s1_b_old { SELECT count(*) FROM broken_hot WHERE b @> ARRAY[1]; }
step s1_b_new { SELECT count(*) FROM broken_hot WHERE b @> ARRAY[2]; }
step s1_commit { COMMIT; }


session s2

setup
{
    SET max_parallel_maintenance_workers = 1;
}

step s2_update { UPDATE broken_hot SET a = 2, b = ARRAY[2]; }
step s2_btree { CREATE INDEX broken_hot_idx ON broken_hot USING btree (a); }
step s2_brin { CREATE INDEX broken_hot_idx ON broken_hot USING brin (a); }
step s2_gin { CREATE INDEX broken_hot_idx ON broken_hot USING gin (b); }

# Only the btree and GIN builds must set indcheckxmin.
step s2_indcheckxmin
{
    SELECT indcheckxmin FROM pg_index
      WHERE indexrelid = 'broken_hot_idx'::regclass;
}

permutation s1_a_old s1_a_new s2_update s2_btree s2_indcheckxmin
  s1_a_old s1_a_new s1_commit
permutation s1_a_old s1_a_new s2_update s2_brin s2_indcheckxmin
  s1_a_old s1_a_new s1_commit
permutation s1_b_old s1_b_new s2_update s2_gin s2_indcheckxmin
  s1_b_old s1_b_new s1_commit
