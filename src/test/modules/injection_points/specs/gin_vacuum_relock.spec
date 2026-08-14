# GIN VACUUM drops its share lock on a posting tree root and re-takes it in
# exclusive mode without re-checking GinPageIsLeaf().  A concurrent root split
# rewrites the page in place with GIN_LEAF cleared, so the page can be internal
# by the time the exclusive lock arrives.  ginVacuumPostingTreeLeaf() then runs
# on an internal page, the rightlink sweep ends at once, and the vacuum reports
# success without having visited a leaf.  The heap pass frees the line pointers
# whose index entries are still present, later rows recycle them, and an index
# scan returns rows that a sequential scan over the same predicate does not.
#
# Both counts below must be 13899: 6000 rows loaded, 101 deleted, 8000 added.
#
# On an assert-enabled build this does not reach the counts.  The first item
# decoded from the internal page is (0,0), and ginVacuumItemPointers() trips
# Assert(ItemPointerIsValid(pointer)), so the backend dies and the rest of the
# isolation suite goes with it.  On a build without asserts the vacuum runs to
# completion and the counts diverge, via_index reporting 14000 against
# via_heap's 13899.
#
# The other two GIN sites doing this share-then-exclusive relock both re-check.
# ginTraverseLock() carries the comment "But root can become non-leaf during
# relock", and ginbulkdelete()'s own entry-tree descent tests
# blkno == GIN_ROOT_BLKNO && !GinPageIsLeaf(page) and restarts.

setup
{
	CREATE EXTENSION injection_points;

	CREATE TABLE gin_relock (id int, tags text[]);
	INSERT INTO gin_relock
		SELECT g, ARRAY['public'] FROM generate_series(1, 6000) g;
	CREATE INDEX gin_relock_idx ON gin_relock USING gin (tags)
		WITH (fastupdate = off);

	-- The entries the VACUUM below is supposed to remove.
	DELETE FROM gin_relock WHERE id BETWEEN 100 AND 200;
}

teardown
{
	DROP TABLE gin_relock;
	DROP EXTENSION injection_points;
}

session s_vacuum
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('gin-vacuum-posting-tree-relock', 'wait');
}
# Stops in the window where no lock is held on the posting tree root.
step vacuum_gin		{ VACUUM gin_relock; }
# Recycles the line pointers the VACUUM freed.
step recycle		{ INSERT INTO gin_relock
					  SELECT 900000 + g, ARRAY['recycled']
					  FROM generate_series(1, 400) g; }
step count_via_index
{
	SET enable_seqscan = off;
	SET enable_indexscan = on;
	SET enable_bitmapscan = on;
	SELECT count(*) AS via_index FROM gin_relock WHERE tags @> ARRAY['public'];
}
step count_via_heap
{
	SET enable_seqscan = on;
	SET enable_indexscan = off;
	SET enable_bitmapscan = off;
	SELECT count(*) AS via_heap FROM gin_relock WHERE tags @> ARRAY['public'];
}

session s_writer
# Splits the posting tree root while the VACUUM holds no lock on it.
step split_root		{ INSERT INTO gin_relock
					  SELECT g, ARRAY['public']
					  FROM generate_series(20001, 28000) g; }
# Detach before waking, so that a VACUUM which correctly notices the page is no
# longer a leaf and restarts its descent does not stop here a second time.
step release
{
	SELECT injection_points_detach('gin-vacuum-posting-tree-relock');
	SELECT injection_points_wakeup('gin-vacuum-posting-tree-relock');
}

permutation vacuum_gin split_root release recycle count_via_index count_via_heap
