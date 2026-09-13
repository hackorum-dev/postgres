# Test REPACK decoding worker startup failures.
setup
{
	CREATE EXTENSION injection_points;

	CREATE TABLE repack_worker_test(i int PRIMARY KEY, j text);
	INSERT INTO repack_worker_test
	SELECT i, repeat(i::text, 100) FROM generate_series(1, 100) AS i;
	CREATE TABLE repack_worker_nodes(phase text, node oid);
	INSERT INTO repack_worker_nodes
	SELECT 'before', relfilenode FROM pg_class
	WHERE oid = 'repack_worker_test'::regclass;
}

teardown
{
	DROP TABLE repack_worker_test;
	DROP TABLE repack_worker_nodes;
	DROP EXTENSION injection_points;
}

session s1
step repack_fail
{
	REPACK (CONCURRENTLY) repack_worker_test
	USING INDEX repack_worker_test_pkey;
}
step repack_retry
{
	REPACK (CONCURRENTLY) repack_worker_test
	USING INDEX repack_worker_test_pkey;
}
step record_failure
{
	INSERT INTO repack_worker_nodes
	SELECT 'failed', relfilenode FROM pg_class
	WHERE oid = 'repack_worker_test'::regclass;
}
step record_success
{
	INSERT INTO repack_worker_nodes
	SELECT 'success', relfilenode FROM pg_class
	WHERE oid = 'repack_worker_test'::regclass;
}
step check
{
	SELECT count(*) AS differences
	FROM
	(
		(SELECT i, j FROM repack_worker_test
		 EXCEPT ALL
		 SELECT i, repeat(i::text, 100) FROM generate_series(1, 100) AS i)
		UNION ALL
		(SELECT i, repeat(i::text, 100) FROM generate_series(1, 100) AS i
		 EXCEPT ALL
		 SELECT i, j FROM repack_worker_test)
	) AS diff;
	SELECT before.node = failed.node AS failure_unchanged,
		   before.node <> success.node AS success_rewritten
	FROM repack_worker_nodes before,
		 repack_worker_nodes failed,
		 repack_worker_nodes success
	WHERE before.phase = 'before' AND failed.phase = 'failed'
		  AND success.phase = 'success';
}

session s2
step fail_before_attach
{
	SELECT injection_points_attach('repack-worker-before-dsm-attach', 'error');
}
step fail_after_attach
{
	SELECT injection_points_attach('repack-worker-after-error-queue-attach', 'error');
}
step detach_before_attach
{
	SELECT injection_points_detach('repack-worker-before-dsm-attach');
}
step detach_after_attach
{
	SELECT injection_points_detach('repack-worker-after-error-queue-attach');
}

# A worker error before DSM and error-queue attachment cannot be propagated,
# so the leader reports a generic initialization failure and can retry.
permutation
	fail_before_attach
	repack_fail
	record_failure
	detach_before_attach
	repack_retry
	record_success
	check

# Once the error queue is attached, preserve the worker's original error.
permutation
	fail_after_attach
	repack_fail
	record_failure
	detach_after_attach
	repack_retry
	record_success
	check
