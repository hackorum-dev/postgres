
# Copyright (c) 2026, PostgreSQL Global Development Group

# BUG #19620: a REINDEX of a non-unique catalog index can emit two btree
# entries for the same heap TID when it races with a HOT update.  Catalog
# writers release their lock before commit, so the index build (which holds
# ShareLock) can still see INSERT_IN_PROGRESS heap-only tuples.  Waiting for
# those inserts would deadlock VACUUM FULL/CLUSTER on catalogs (1ddc2703);
# heapam_index_build_range_scan instead skips a second emission of the same
# root TID.
#
# pg_class_tblspc_relfilenode_index is the usual casualty: it is not unique,
# so the build does not wait on INSERT_IN_PROGRESS.
#
# The race is easy to miss on an empty initdb (tiny pg_class, scan finishes
# in microseconds).  Inflate pg_class, then hammer GRANT/REVOKE on one
# persistent row (relacl is not indexed, so the update is HOT) against
# REINDEX.  Unpatched cassert traps in comparetup_index_btree_tiebreak
# ("ItemPointer values should never be equal").

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

my $node = PostgreSQL::Test::Cluster->new('catalog_reindex_hot');
$node->init;
$node->append_conf('postgresql.conf',
	'lock_timeout = ' . (1000 * $PostgreSQL::Test::Utils::timeout_default));
$node->append_conf('postgresql.conf', 'deadlock_timeout = 1s');
$node->append_conf('postgresql.conf', 'max_locks_per_transaction = 128');
$node->start;
$node->safe_psql('postgres', q(CREATE EXTENSION amcheck));

# Extra pg_class rows lengthen the index-build heap scan.  CREATE TYPE AS ()
# is a cheap pg_class insert; COMMIT every batch so we do not exhaust the
# lock table.  persist_1 is the HOT target (relacl).
$node->safe_psql(
	'postgres',
	q(
CREATE TABLE persist_1(i int);
CREATE PROCEDURE catalog_hot_bloat(n_types int)
LANGUAGE plpgsql AS $$
DECLARE
	i int;
BEGIN
	FOR i IN 1..n_types LOOP
		EXECUTE format('CREATE TYPE catalog_hot_ty_%s AS ()', i);
		IF i % 40 = 0 THEN
			COMMIT;
		END IF;
	END LOOP;
	COMMIT;
END;
$$;
CALL catalog_hot_bloat(8000);
));

#
# VACUUM FULL must not wait out an in-progress catalog insert.  CREATE TABLE
# inserts into pg_class and then releases that lock before commit; the rewrite
# should finish while the inserting transaction is still open.
#
# While it runs, the inserting backend must still be able to do more catalog
# work.  Waiting for INSERT_IN_PROGRESS during the rewrite deadlocks here:
# VACUUM FULL holds AccessExclusiveLock and waits for the inserter; the
# inserter waits for that exclusive lock.
#
my $hold = $node->background_psql('postgres');
$hold->query_safe(q(BEGIN; CREATE TABLE hold_open(i int);));

my $vf = $node->background_psql('postgres');
$vf->query_until(
	qr/start/,
	q(
\echo start
SET statement_timeout = '30s';
VACUUM FULL pg_class;
));
$hold->query_safe(q(CREATE TABLE hold_open_2(i int);));
$vf->query_safe(q(SELECT 1));
pass('VACUUM FULL pg_class does not deadlock with in-progress catalog inserts');

$hold->query_safe(q(COMMIT; DROP TABLE hold_open, hold_open_2;));
$hold->quit;
$vf->quit;

#
# Stress REINDEX against concurrent catalog HOT updates of a single pg_class
# row.  Serialize GRANT vs GRANT with an advisory lock so pgbench is not
# aborted by "tuple concurrently updated"; REINDEX (ShareLock) still races
# with GRANT (RowExclusiveLock).
#
$node->pgbench(
	'--no-vacuum --client=4 --jobs=4 --time=12',
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent catalog REINDEX and HOT GRANT',
	{
		'007_reindex_catalog' => q(
			REINDEX INDEX pg_class_tblspc_relfilenode_index;
		),
		'007_catalog_acl_hot' => q(
			SELECT pg_try_advisory_lock(43)::integer AS gotlock \gset
			\if :gotlock
				GRANT SELECT ON persist_1 TO PUBLIC;
				REVOKE SELECT ON persist_1 FROM PUBLIC;
				SELECT pg_advisory_unlock(43);
			\endif
		)
	});

$node->safe_psql(
	'postgres',
	q(
SELECT bt_index_check('pg_class_tblspc_relfilenode_index', true);
SELECT bt_index_check('pg_class_relname_nsp_index', true);
SELECT bt_index_check('pg_class_oid_index', true);
SELECT bt_index_parent_check('pg_class_tblspc_relfilenode_index', true, true);
));
pass('pg_class indexes pass bt_index_check after concurrent REINDEX');

$node->stop;
done_testing();
