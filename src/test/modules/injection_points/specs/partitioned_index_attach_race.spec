# Deterministic reproducer for the concurrent partitioned-index attach
# race behind the buildfarm serinus "pg_restore: there is no unique
# constraint matching given keys for referenced table" failure
# (parallel restore of multi-level partitioned PK trees).
#
# Mechanism under test (see validatePartitionedIndex() /
# ATExecAttachPartitionIdx() in tablecmds.c):
#
# pg_dump emits the parent constraint as "ALTER TABLE ONLY ... ADD
# PRIMARY KEY", which declines recursion; since the partitions are
# attached already, DefineIndex() creates the partitioned index
# invalid (INDEX_CREATE_INVALID).  Such an index can only become
# valid again through validatePartitionedIndex(), which counts the
# children found valid *with the counting statement's own snapshot*.
# If two concurrent transactions complete two different sub-chains of
# the same parent index, each one counts the other's not-yet-committed
# flag change as still invalid, so neither counts the complete set:
# the parent index remains indisvalid=false forever, although every
# index of the tree ends up attached and valid.  Any later statement
# that requires the parent index to be valid -- e.g. ADD FOREIGN KEY
# referencing the partitioned table -- then fails with "there is no
# unique constraint matching given keys".
#
# The interleaving is driven by two mechanisms:
#   - the injection point "alter-index-attach-complete" (end of
#     ATExecAttachPartitionIdx(), before commit) parks the s1
#     statement right after its validation decisions; it is attached
#     with a condition matching the parent index name "partb_pkey",
#     so the s2 statement (and the healing statement at the end)
#     pass through it freely;
#   - session ctrl holds an ACCESS EXCLUSIVE lock on table partc1,
#     so the s2 statement blocks on a heavyweight lock until it is
#     released; a DO block waits until s1 is provably parked at the
#     injection point (visible in pg_stat_activity.wait_event)
#     before that, making the ordering fully deterministic.
#
# Timeline:
#   s1: ALTER INDEX partb_pkey ATTACH PARTITION partb1_pkey
#       -> partb_pkey becomes valid, recurse-up counts top_pkey:
#       2 of 3 (partc_pkey still invalid, s2 has not started),
#       parks at the injection point, transaction not committed.
#   ctrl verifies s1 is parked, releases the lock.
#   s2: ALTER INDEX partc_pkey ATTACH PARTITION partc1_pkey
#       -> partc_pkey becomes valid, recurse-up counts top_pkey:
#       2 of 3 (partb_pkey not visible: s1's transaction is still
#       open), commits.  top_pkey is now permanently invalid.
#   s1 is woken up, commits.
#   test_fk_fail: ADD FOREIGN KEY fails.
#   Re-issuing one ATTACH after everything has committed heals the
#   index, and the FK then works.

setup
{
	CREATE EXTENSION injection_points;

	CREATE TABLE top (a int NOT NULL) PARTITION BY RANGE (a);
	CREATE TABLE parta (a int NOT NULL PRIMARY KEY);
	CREATE TABLE partb (a int NOT NULL) PARTITION BY RANGE (a);
	CREATE TABLE partb1 (a int NOT NULL PRIMARY KEY);
	CREATE TABLE partc (a int NOT NULL) PARTITION BY RANGE (a);
	CREATE TABLE partc1 (a int NOT NULL PRIMARY KEY);
	ALTER TABLE top ATTACH PARTITION parta FOR VALUES FROM (0) TO (1);
	ALTER TABLE top ATTACH PARTITION partb FOR VALUES FROM (1) TO (2);
	ALTER TABLE partb ATTACH PARTITION partb1 FOR VALUES FROM (1) TO (2);
	ALTER TABLE top ATTACH PARTITION partc FOR VALUES FROM (2) TO (3);
	ALTER TABLE partc ATTACH PARTITION partc1 FOR VALUES FROM (2) TO (3);

	-- partitioned PK indexes of the two sub-partitioned branches,
	-- born invalid (recursion declined by ONLY while partitions
	-- exist)
	ALTER TABLE ONLY partb ADD CONSTRAINT partb_pkey PRIMARY KEY (a);
	ALTER TABLE ONLY partc ADD CONSTRAINT partc_pkey PRIMARY KEY (a);
	-- the parent PK, also born invalid, as pg_dump restores it
	ALTER TABLE ONLY top ADD CONSTRAINT top_pkey PRIMARY KEY (a);

	-- the direct leaf index is valid; the top index stays invalid
	-- (1 valid child of 3)
	ALTER INDEX top_pkey ATTACH PARTITION parta_pkey;
	-- attach the (still invalid) mid-level indexes; top stays invalid
	ALTER INDEX top_pkey ATTACH PARTITION partb_pkey;
	ALTER INDEX top_pkey ATTACH PARTITION partc_pkey;

	CREATE TABLE fk (a int);

	SELECT injection_points_attach('alter-index-attach-complete',
	                                'wait', 'partb_pkey');
}

teardown
{
	SELECT injection_points_detach('alter-index-attach-complete');
	DROP TABLE fk, top CASCADE;
	DROP EXTENSION injection_points;
}

session s1
step s1_attach_partb1
{
	ALTER INDEX partb_pkey ATTACH PARTITION partb1_pkey;
}
step s1_barrier
{
	SELECT 1 AS s1_done;
}

session s2
step s2_attach_partc1
{
	ALTER INDEX partc_pkey ATTACH PARTITION partc1_pkey;
}
step s2_barrier
{
	SELECT 1 AS s2_done;
}

session ctrl
step ctrl_lock
{
	BEGIN;
	LOCK TABLE partc1 IN ACCESS EXCLUSIVE MODE;
}
# Wait until s1 is parked at the injection point (its statement
# runs while s2 has not started, so s1's validation necessarily
# counts only 2 of 3 children of top_pkey).
step ctrl_gate
{
	DO $$
	DECLARE
		n integer := 0;
	BEGIN
		WHILE NOT EXISTS
		      (SELECT 1 FROM pg_stat_activity
		        WHERE wait_event = 'alter-index-attach-complete')
		LOOP
			PERFORM pg_sleep(0.01);
			n := n + 1;
			IF n > 3000 THEN
				RAISE EXCEPTION 's1 never reached the injection point';
			END IF;
		END LOOP;
	END
	$$;
}
step ctrl_release
{
	ROLLBACK;
}
step ctrl_wakeup
{
	SELECT injection_points_wakeup('alter-index-attach-complete');
}

session test
step test_state_broken
{
	SELECT c.relname, i.indisvalid
	  FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
	 WHERE c.relname IN ('top_pkey', 'parta_pkey', 'partb_pkey',
	                     'partb1_pkey', 'partc_pkey', 'partc1_pkey')
	 ORDER BY c.relname;
}
step test_fk_fail
{
	ALTER TABLE fk ADD CONSTRAINT fk_a_fkey FOREIGN KEY (a) REFERENCES top(a);
}
# After everything has committed, one more (idempotent) ATTACH runs
# one validation round and heals the index -- the shape of the
# scheduling fix proposed for pg_dump/pg_restore.  Its parent index
# is top_pkey, so the injection point does not wait for it.
step test_heal
{
	ALTER INDEX top_pkey ATTACH PARTITION parta_pkey;
}
step test_fk_ok
{
	ALTER TABLE fk ADD CONSTRAINT fk_a_fkey FOREIGN KEY (a) REFERENCES top(a);
}

permutation
	# s1 completes and validates partb_pkey, recurses up to top_pkey
	# (counting 2 of 3), then parks at the injection point with its
	# transaction open; "(*)" makes the tester treat it as blocked
	# immediately
	s1_attach_partb1(*)
	# keep s2 out of the picture, then wait until s1 is provably
	# parked at the injection point
	ctrl_lock
	ctrl_gate
	# s2 blocks on the heavyweight lock (native wait detection)
	s2_attach_partc1
	# releasing the lock lets s2 run while s1's transaction is still
	# open: it counts 2 of 3 as well, and commits, leaving top_pkey
	# permanently invalid
	ctrl_release
	s2_barrier
	# release s1; its transaction commits
	ctrl_wakeup
	s1_barrier
	# the parent index is now permanently invalid...
	test_state_broken
	# ...so ADD FOREIGN KEY fails, although every leaf index is valid
	test_fk_fail
	# one extra idempotent attach after everything has committed
	# heals the parent index
	test_heal
	# and the FK now works
	test_fk_ok
