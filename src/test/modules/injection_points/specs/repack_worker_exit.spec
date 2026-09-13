# Exercise each post-attachment wait, then retry in the same backend.
setup
{
	CREATE EXTENSION injection_points;
	SELECT injection_points_attach('repack-worker-after-error-queue-attach',
		'injection_points', 'injection_exit', NULL);
	SELECT injection_points_attach('repack-worker-before-snapshot-export',
		'injection_points', 'injection_exit', NULL);
	SELECT injection_points_attach('repack-worker-before-replay-export',
		'injection_points', 'injection_exit', NULL);
	CREATE TABLE repack_worker_exit(i int PRIMARY KEY, j text);
	INSERT INTO repack_worker_exit VALUES (1, 'one'), (2, 'two'), (3, 'three');
	CREATE TABLE original_node AS
		SELECT pg_relation_filenode('repack_worker_exit') AS node;
}

teardown
{
	DROP TABLE repack_worker_exit, original_node;
	DROP EXTENSION injection_points;
}

session s1
step repack { REPACK (CONCURRENTLY) repack_worker_exit; }
step init_error
{
	SELECT injection_points_detach('repack-worker-after-error-queue-attach');
	SELECT injection_points_attach('repack-worker-after-error-queue-attach', 'error');
}
step detach_init
{
	SELECT injection_points_detach('repack-worker-after-error-queue-attach');
}
step snapshot_error
{
	SELECT injection_points_detach('repack-worker-before-snapshot-export');
	SELECT injection_points_attach('repack-worker-before-snapshot-export', 'error');
}
step detach_snapshot
{
	SELECT injection_points_detach('repack-worker-before-snapshot-export');
}
step detach_replay
{
	SELECT injection_points_detach('repack-worker-before-replay-export');
}
step check_failure
{
	SELECT pg_relation_filenode('repack_worker_exit') = node AS failure_unchanged
	FROM original_node;
}
step check_success
{
	SELECT pg_relation_filenode('repack_worker_exit') <> node AS success_rewritten
	FROM original_node;
}
step check_data { SELECT * FROM repack_worker_exit ORDER BY i; }

# A queued error must retain priority over the generic stopped-worker error.
permutation
	repack init_error
	repack detach_init
	repack snapshot_error
	repack detach_snapshot
	repack detach_replay check_failure check_data
	repack check_success check_data
