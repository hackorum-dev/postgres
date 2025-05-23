# Test that pg_class.reltuples is updated with the correct value after
# a VACUUM FULL when there are concurrent transactions preventing
# removal of some garbage tuples. The reltuples count should not
# include these garbage tuples.

setup
{
	CREATE TABLE stats (a INT);
    INSERT INTO stats SELECT generate_series(1, 10);
}

teardown
{
	DROP TABLE IF EXISTS stats;
}

# Session 1 (s1) keeps an open transaction in repeatable read to
# prevent VACUUM FULL in session 2 (s2) from removing some dead tuples
# that session 1 should still see in its snapshot. Note that s1 needs
# to run a query to initiate a snapshot.
session s1
step s1_query
{
	START TRANSACTION ISOLATION LEVEL REPEATABLE READ;
    SELECT 1;
}
step s1_commit
{
    COMMIT;
}

session s2
step s2_update       { UPDATE stats SET a = 1 WHERE a > 6; } 
step s2_vac_full     { VACUUM FULL stats; }
step s2_reltuples    { SELECT reltuples FROM pg_class WHERE relname = 'stats'; }
step s2_create_index { CREATE INDEX ON stats (a); }

permutation s2_vac_full s2_reltuples s1_query s2_update s2_reltuples s2_vac_full s1_commit s2_reltuples

# If the table has at least one index, then the index rebuild will set
# reltuples after VACUUM FULL, so run a test with that too.
permutation s2_create_index s2_vac_full s2_reltuples s1_query s2_update s2_reltuples s2_vac_full s1_commit s2_reltuples
