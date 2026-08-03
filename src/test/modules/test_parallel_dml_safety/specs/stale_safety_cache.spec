# Regression test for stale cache write-after-invalidation in
# RelationGetParallelDmlSafety().
#
# The computation of a partitioned table's hazard level opens (locks) each
# partition in turn, and acquiring a lock can process pending invalidation
# messages.  If a concurrent commit makes an already-examined partition
# parallel-unsafe while the computation is blocked on the next partition's
# lock, the invalidation is processed in the middle of the computation: the
# result computed so far is based on the pre-change catalogs, and must not
# be cached.  rd_paralleldmlstate detects that and the computation is
# redone; without that, the session would keep a stale "safe" value and
# plan parallel INSERTs into a relation that is no longer parallel-safe,
# which then fail at execution time.

setup
{
	CREATE EXTENSION test_parallel_dml_safety;

	CREATE TABLE stale_root (a int) PARTITION BY RANGE (a);
	CREATE TABLE stale_p1 PARTITION OF stale_root FOR VALUES FROM (0) TO (50);
	CREATE TABLE stale_p2 PARTITION OF stale_root FOR VALUES FROM (50) TO (100);

	CREATE TABLE stale_src (a int);
	INSERT INTO stale_src SELECT g % 50 FROM generate_series(1, 1000) g;

	CREATE FUNCTION stale_unsafe() RETURNS trigger
		LANGUAGE plpgsql PARALLEL UNSAFE
		AS $$ BEGIN RETURN NEW; END $$;
}

teardown
{
	DROP TABLE stale_root, stale_src;
	DROP FUNCTION stale_unsafe();
	DROP EXTENSION test_parallel_dml_safety;
}

# The session whose cache is invalidated mid-computation.
session s1
setup			{ SET min_parallel_table_scan_size = 0;
				  SET parallel_setup_cost = 0;
				  SET parallel_tuple_cost = 0;
				  SET max_parallel_workers_per_gather = 2; }
step s1_compute	{ SELECT test_parallel_dml_safety('stale_root'::regclass); }
step s1_cached	{ SELECT test_parallel_dml_safety_cached('stale_root'::regclass); }
step s1_recheck	{ SELECT test_parallel_dml_safety('stale_root'::regclass); }
step s1_insert	{ INSERT INTO stale_root SELECT a FROM stale_src; }

# The session that makes the relation parallel unsafe while s1 is computing.
session s2
setup			{ BEGIN; }
step s2_lock	{ LOCK TABLE stale_p2 IN ACCESS EXCLUSIVE MODE; }
# CREATE TRIGGER takes ShareRowExclusiveLock, which does not conflict with
# the AccessShareLock s1 already holds on stale_p1, so s1 has read stale_p1
# as safe by the time this runs.
step s2_trig	{ CREATE TRIGGER stale_trig BEFORE INSERT ON stale_p1
					FOR EACH ROW EXECUTE FUNCTION stale_unsafe(); }
step s2_commit	{ COMMIT; }

# A session that never saw the pre-change state, for comparison.
session s3
setup			{ SET min_parallel_table_scan_size = 0;
				  SET parallel_setup_cost = 0;
				  SET parallel_tuple_cost = 0;
				  SET max_parallel_workers_per_gather = 2; }
step s3_truth	{ SELECT test_parallel_dml_safety('stale_root'::regclass); }
step s3_insert	{ INSERT INTO stale_root SELECT a FROM stale_src; }

# s1_compute blocks on stale_p2 after having examined stale_p1 as safe; s2
# then makes stale_p1 unsafe and commits.  s1 processes the invalidation
# while waking up, so the computation is redone and both returns and caches
# 'u', like the fresh session s3 computes; both INSERTs then get a
# non-parallel plan and succeed.  (Before the fix, s1 returned and cached
# the stale 's', and its INSERT failed at execution time when the executor
# rechecked the partition.)
permutation s2_lock s1_compute s2_trig s2_commit s1_cached s1_recheck s3_truth s1_insert s3_insert
