# Test that inserting into a partition that has become parallel-unsafe after
# the statement started execution fails, rather than running with a stale
# safety assumption.
#
# The inserter runs an INSERT into a partitioned table in parallel mode
# (forced with debug_parallel_query; the plan has no Gather, so everything
# runs in the leader, in a deterministic row order).  Evaluation of the
# first row waits for an advisory lock held by the other session.  While
# the inserter is blocked, the other session adds a parallel-unsafe trigger
# to partition pi_p1 and commits.  When the inserter wakes up, it processes
# the invalidation messages (during the lock wait), so when the next row is
# routed to pi_p1, the execution-time check in ExecInitPartitionInfo
# detects that pi_p1 is no longer parallel-safe and errors out.

setup
{
  CREATE FUNCTION pi_unsafe_fn() RETURNS trigger
    LANGUAGE plpgsql PARALLEL UNSAFE AS $$
  BEGIN
    RETURN NEW;
  END $$;

  CREATE TABLE pi_root (a int) PARTITION BY RANGE (a);
  CREATE TABLE pi_p1 PARTITION OF pi_root FOR VALUES FROM (0) TO (50);
  CREATE TABLE pi_p2 PARTITION OF pi_root FOR VALUES FROM (50) TO (100);
}

teardown
{
  DROP TABLE pi_root;
  DROP FUNCTION pi_unsafe_fn();
}

session inserter
step iset	{ SET debug_parallel_query = on; }
# the first row (50, routed to pi_p2) waits for the advisory lock; the
# second row (1, routed to pi_p1) is evaluated afterwards
step iins	{ INSERT INTO pi_root
			  SELECT CASE WHEN i = 1 THEN 50 ELSE 1 END
			  FROM generate_series(1, 2) i
			  WHERE (i <> 1 OR pg_advisory_lock(1) IS NOT NULL); }

session locker
step lb		{ BEGIN; }
step llock	{ SELECT pg_advisory_xact_lock(1); }
step ltrg	{ CREATE TRIGGER trg_bad BEFORE INSERT ON pi_p1
			  FOR EACH ROW EXECUTE FUNCTION pi_unsafe_fn(); }
step lc		{ COMMIT; }

permutation lb llock iset iins ltrg lc
