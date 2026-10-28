# A snapshot older than a not-null constraint can still see tuples with
# nulls in the constrained column.  Neither the planner nor the tuple
# deforming fast path may rely on the constraint for such a snapshot.

setup
{
  CREATE TABLE nn_snap (id int PRIMARY KEY, a int, b int, c text);
  INSERT INTO nn_snap SELECT g, CASE WHEN g <= 5 THEN NULL ELSE g * 10 END,
    g + 100, 'c' || g FROM generate_series(1, 10) g;
  ALTER TABLE nn_snap ADD CONSTRAINT nn_snap_a_nn NOT NULL a NOT VALID;
}

teardown
{
  DROP TABLE nn_snap;
}

session s1
step s1_begin		{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM nn_snap WHERE a IS NULL; }
step s1_snap		{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1; }
step s1_isnull		{ SELECT count(*) FROM nn_snap WHERE a IS NULL; }
step s1_counta		{ SELECT count(a), count(*) FROM nn_snap; }
step s1_cols		{ SELECT id, a, b FROM nn_snap WHERE id <= 5; }
step s1_coalesce	{ SELECT id, coalesce(a, -1) FROM nn_snap WHERE id <= 5; }
step s1_commit		{ COMMIT; }

session s2
step s2_validate	{ UPDATE nn_snap SET a = 0 WHERE a IS NULL; ALTER TABLE nn_snap VALIDATE CONSTRAINT nn_snap_a_nn; }
step s2_setnotnull	{ ALTER TABLE nn_snap DROP CONSTRAINT nn_snap_a_nn; UPDATE nn_snap SET a = 0 WHERE a IS NULL; ALTER TABLE nn_snap ALTER a SET NOT NULL; }

# constraint validated after s1 took its snapshot and read the table
permutation s1_begin s2_validate s1_isnull s1_counta s1_cols s1_coalesce s1_commit
# constraint created after s1 took its snapshot
permutation s1_snap s2_setnotnull s1_isnull s1_counta s1_cols s1_coalesce s1_commit
