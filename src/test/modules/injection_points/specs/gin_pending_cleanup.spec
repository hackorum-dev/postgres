# ginbulkdelete() drained the GIN pending list only on VACUUM's first index
# pass, because the call sat inside its "if (stats == NULL)" block.  The set of
# TIDs to delete is rebuilt for every pass, though, so a later pass could be
# handed a TID whose only index entry was still sitting in the pending list.
# That pass swept the entry tree and the posting trees, left the pending entry
# alone, and the heap pass then freed the line pointer underneath it.
#
# A tuple only has to become dead after the first pass to reach this.  An
# aborted UPDATE does that at once: HeapTupleSatisfiesVacuum() reports
# HEAPTUPLE_DEAD for an aborted xmin whatever OldestXmin is, and the pending
# list entry aminsert already wrote is not rolled back with it.
#
# Both counts below must be 0.  Without the fix the index keeps an entry for a
# line pointer that VACUUM freed, a later row recycles that line pointer, and
# via_index reports the recycled rows while via_heap reports none.  Nothing
# repairs the index afterwards: VACUUM only removes TIDs it collected, and this
# one is LP_UNUSED by then.
#
# The setup deliberately makes VACUUM need several index passes: the dead line
# pointers cover far more pages than maintenance_work_mem can track.  Step
# scan_incomplete asserts that this still holds, so that the test cannot go on
# passing once it has stopped reaching a second pass.

setup
{
	CREATE EXTENSION injection_points;

	CREATE TABLE gin_pending (id int, tags text[])
		WITH (autovacuum_enabled = off);
	INSERT INTO gin_pending
		SELECT g, ARRAY['v' || g] FROM generate_series(1, 12000) g;
	CREATE INDEX gin_pending_idx ON gin_pending USING gin (tags)
		WITH (fastupdate = on, gin_pending_list_limit = 65536);

	-- Free space on the last pages, so that the aborted UPDATE below keeps its
	-- new row version on the page the row is already on.
	DELETE FROM gin_pending WHERE id > 11400 AND id % 2 = 0;
}

# VACUUM cannot run alongside other statements in one setup block.  INDEX_CLEANUP
# ON so that the bypass cannot skip freeing these line pointers, which is what
# leaves the free space the aborted UPDATE below relies on.
setup	{ VACUUM (INDEX_CLEANUP ON) gin_pending; }

setup
{
	-- The dead line pointers the VACUUM below is supposed to remove.  They
	-- span more pages than maintenance_work_mem can hold at once.
	DELETE FROM gin_pending WHERE id <= 11400 AND id % 4 <> 0;
}

teardown
{
	DROP TABLE gin_pending;
	DROP EXTENSION injection_points;
}

session s_vacuum
setup
{
	SET maintenance_work_mem = '64kB';
	SELECT injection_points_set_local();
	SELECT injection_points_attach('gin-bulkdelete-pending-cleaned', 'wait');
}
# Stops once the first index pass has drained the pending list.
step vacuum_gin		{ VACUUM (INDEX_CLEANUP ON) gin_pending; }
# Recycles the line pointers the VACUUM freed.
step recycle		{ INSERT INTO gin_pending
					  SELECT 900000 + g, ARRAY['recycled']
					  FROM generate_series(1, 12000) g; }
step count_via_index
{
	SET enable_seqscan = off;
	SET enable_indexscan = on;
	SET enable_bitmapscan = on;
	SELECT count(*) AS via_index FROM gin_pending WHERE tags @> ARRAY['ghost'];
}
step count_via_heap
{
	SET enable_seqscan = on;
	SET enable_indexscan = off;
	SET enable_bitmapscan = off;
	SELECT count(*) AS via_heap FROM gin_pending WHERE tags @> ARRAY['ghost'];
}

session s_writer
# The heap scan must still have pages left, or there is no second index pass
# and no page left holding the row the ghost below lands on.
step scan_incomplete
{
	SELECT heap_blks_scanned < heap_blks_total AS scan_incomplete
	FROM pg_stat_progress_vacuum WHERE relid = 'gin_pending'::regclass;
}
step ghost_begin	{ BEGIN; }
# Leaves a dead row version, and a pending list entry for it, on a page the
# heap scan has not reached yet.
step ghost_update	{ UPDATE gin_pending SET tags = ARRAY['ghost'] WHERE id > 11900; }
step ghost_rollback	{ ROLLBACK; }
# Detach before waking, so the remaining index passes do not stop here again.
step release
{
	SELECT injection_points_detach('gin-bulkdelete-pending-cleaned');
	SELECT injection_points_wakeup('gin-bulkdelete-pending-cleaned');
}

permutation vacuum_gin scan_incomplete ghost_begin ghost_update ghost_rollback release recycle count_via_index count_via_heap
