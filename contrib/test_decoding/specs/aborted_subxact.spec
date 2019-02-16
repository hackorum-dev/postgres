# Test DDL in aborted subxact

setup
{
    SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'test_decoding'); -- must be first write in xact
    CREATE TABLE harvest(apples int);
}

teardown
{
    DROP TABLE IF EXISTS harvest;
    SELECT 'stop' FROM pg_drop_replication_slot('isolation_slot');
}

session "s0"
setup { SET synchronous_commit=on; }
step "s0_begin" { BEGIN; select 'harvest'::regclass::int; }
step "s0_alter_0" { SAVEPOINT a; ALTER TABLE harvest ADD COLUMN pears int; ROLLBACK TO SAVEPOINT a; }
step "s0_alter_1" { ALTER TABLE harvest ADD COLUMN peaches text; }
step "s0_insert" { INSERT INTO harvest values (42, 'red'); }
step "s0_alter_2" { ALTER TABLE harvest ADD COLUMN pears int; }
step "s0_commit" { COMMIT; }
step "s0_get_changes" { SELECT data FROM pg_logical_slot_get_changes('isolation_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1'); }

session "s1"
setup { SET synchronous_commit=on; }
step "s1_vacuum" { VACUUM pg_attribute; }
step "s1_vacuum_full" { VACUUM FULL; }

# alter_0 adds row in pg_attribute for which we remember cmin = 0; it is
# reclaimed by vacuum
# alter_1 ensures we decode insert with current cid = 1
# alter_2 inserts into another row in place where row of alter_0 was, its cmin
# is 3, however we already remembered it as 0
permutation "s1_vacuum" "s0_begin" "s0_alter_0" "s0_alter_1" "s0_insert" "s1_vacuum" "s0_alter_2" "s0_commit" "s0_get_changes"
