setup
{
    CREATE TABLE test_vacuum_cancel(data text);
    /* don't want autovacuum clean up to-be-cleaned data concurrently */
    ALTER TABLE test_vacuum_cancel SET (AUTOVACUUM_ENABLED = false);
    /* insert some data  */
    INSERT INTO test_vacuum_cancel VALUES('somedata'),('otherdata');
}

teardown
{
    DROP TABLE test_vacuum_cancel;
}

session "s1"
step "s1_begin" { BEGIN; }
step "s1_block_vacuum" { LOCK pg_class IN SHARE MODE; }
step "s1_commit" { COMMIT; }

session "s2"
step "s2_delete" { DELETE FROM test_vacuum_cancel; }
step "s2_vacuum_interruptible" { VACUUM (FREEZE, INTERRUPTIBLE) test_vacuum_cancel; }
step "s2_analyze_interruptible" { ANALYZE (INTERRUPTIBLE) test_vacuum_cancel; }

session "s3"
step "s3_lock" { BEGIN; LOCK test_vacuum_cancel; }
step "s3_commit" { COMMIT; }

# First test if releasing the lock on pg_class allows to continue
permutation
    # block vacuum
    "s1_begin" "s1_block_vacuum" "s2_delete"
    # vacuum, which will be blocked by the above
    "s2_vacuum_interruptible"
    # and allow to continue
    "s1_commit"
permutation "s1_begin" "s1_block_vacuum" "s2_delete" "s2_analyze_interruptible" "s1_commit"

# Then track that concurrent LOCK interrupts VACUUM / ANALYZE
permutation
    # block vacuum
    "s1_begin" "s1_block_vacuum" "s2_delete"
    # vacuum, which will be blocked by the above
    "s2_vacuum_interruptible"
    # issue lock request conflicting with vacuum
    "s3_lock"
    # and release the blocking lock request again. Done separately, to
    # ensure isolationtester schedules predictably
    "s3_commit" "s1_commit"

permutation
    "s1_begin" "s1_block_vacuum" "s2_delete"
    "s2_analyze_interruptible"
    "s3_lock"
    "s3_commit" "s1_commit"
