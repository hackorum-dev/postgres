# INSERT ... ON CONFLICT test verifying that speculative insertion
# failures are handled

setup
{
    CREATE TABLE upserttest(key text, data text);
    CREATE UNIQUE INDEX ON upserttest(key);

    CREATE EXTENSION faultinjector;
    -- start with a clean slate
    SELECT * FROM inject_fault('all', 'reset');

    -- inject fault to suspend insert transaction after a tuple has
    -- been inserted into the heap but before it is inserted into the
    -- index.
    SELECT * FROM inject_fault('insert_index_tuples', 'suspend');
    SELECT * FROM inject_fault('heap_abort_speculative', 'skip');
}

teardown
{
    DROP TABLE upserttest;
    SELECT * FROM inject_fault('all', 'reset');
}


session "s1"
setup
{
  SET default_transaction_isolation = 'read committed';
}
step "s1_upsert" { INSERT INTO upserttest(key, data) VALUES('k1', 'inserted s1') ON CONFLICT (key) DO UPDATE SET data = upserttest.data || ' with conflict update s1'; }
step "s1_fault_status" { SELECT * FROM inject_fault('heap_abort_speculative', 'status'); }
step "s1_select" { SELECT * FROM upserttest; }

session "s2"
setup
{
  SET default_transaction_isolation = 'read committed';
}
step "s2_upsert" { INSERT INTO upserttest(key, data) VALUES('k1', 'inserted s2') ON CONFLICT (key) DO UPDATE SET data = upserttest.data || ' with conflict update s2'; }
step "s2_unblock_s1" { SELECT * FROM inject_fault('insert_index_tuples', 'resume'); }

# Test that speculative locks are correctly acquired and released, s2
# inserts, s1 updates.
permutation
   # S1 should hit the fault and block
   "s1_upsert"&
   # S2 should insert without conflict
   "s2_upsert"
   "s2_unblock_s1"
   "s1_fault_status"
   "s1_select"
