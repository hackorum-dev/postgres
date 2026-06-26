# Verify pg_stat_progress_cluster.index_rebuild_count during the table-scan
# phase of CLUSTER.
#
# A CLUSTER (REPACK) on a table that has a TOAST relation builds the new
# table's TOAST index in make_new_heap(), before the heap is scanned.  That
# internal index build must not report CREATE INDEX progress into the
# enclosing REPACK command: the two commands share progress slot 9
# (PROGRESS_CREATEIDX_PHASE vs PROGRESS_REPACK_INDEX_REBUILD_COUNT), so an
# unsuppressed build leaves index_rebuild_count looking like a CREATE INDEX
# phase value while the cluster is still scanning the heap.  It must read 0
# until indexes are actually rebuilt at the end.

setup
{
	CREATE EXTENSION injection_points;

	-- A table with a TOAST relation (wide, toastable column) and an index to
	-- cluster on.
	CREATE TABLE cluster_progress (i int, t text);
	INSERT INTO cluster_progress
		SELECT g, repeat(md5(g::text), 1000) FROM generate_series(1, 5) g;
	CREATE INDEX cluster_progress_i ON cluster_progress (i);
}

teardown
{
	DROP TABLE cluster_progress;
	DROP EXTENSION injection_points;
}

session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('heap-cluster-scan-start', 'wait');
}
step s1_cluster	{ CLUSTER cluster_progress USING cluster_progress_i; }
teardown		{ SELECT injection_points_detach('heap-cluster-scan-start'); }

session s2
# CLUSTER is suspended at the start of the heap scan; index_rebuild_count must
# be 0 here (indexes are rebuilt only at the end).
step s2_progress
{
	SELECT relid::regclass, phase, index_rebuild_count
	FROM pg_stat_progress_cluster;
}
step s2_wakeup	{ SELECT injection_points_wakeup('heap-cluster-scan-start'); }

permutation s1_cluster s2_progress s2_wakeup
