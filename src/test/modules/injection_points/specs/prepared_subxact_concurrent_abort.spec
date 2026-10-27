# Concurrent abort of a prepared transaction whose changes all belong to a
# subtransaction.
#
# Decoding a prepared transaction at PREPARE time must notice if the
# transaction is rolled back while it is being decoded, because the catalog
# versions it sees can no longer be trusted.  ReorderBufferProcessTXN arms
# that check by calling SetupCheckXidLive() for each change.  Here the
# transaction's catalog change and the DML after it are made inside a
# subtransaction, with no change of the top-level transaction, so the check
# has to be armed for the subtransaction's changes.
#
# s1 decodes the prepared transaction and is held at its first change by an
# injection point.  Meanwhile s2 rolls the transaction back and updates the
# same pg_class row that the aborted ALTER TABLE had updated, which
# overwrites the xmax of the old row version left by the aborted transaction.

setup
{
	CREATE EXTENSION injection_points;
	CREATE TABLE t (a int);
}

# Slot creation cannot share a transaction with writes.
setup
{
	SELECT 'init' FROM pg_create_logical_replication_slot('s', 'test_decoding', false, true);
}

teardown
{
	DROP TABLE t;
	DROP EXTENSION injection_points;
	SELECT pg_drop_replication_slot('s');
}

session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('reorderbuffer-process-change', 'wait');
}
step s1_decode
{
	SELECT data FROM pg_logical_slot_get_changes('s', NULL, NULL,
		'skip-empty-xacts', '1', 'include-xids', '0');
}

session s2
step s2_prepare
{
	BEGIN;
	SAVEPOINT sp;
	ALTER TABLE t ADD COLUMN b int;
	INSERT INTO t VALUES (1, 1);
	RELEASE SAVEPOINT sp;
	PREPARE TRANSACTION 'x';
}
step s2_rollback_prepared	{ ROLLBACK PREPARED 'x'; }
# Update the pg_class row of t that the aborted transaction had updated.
step s2_update_catalog		{ ALTER TABLE t SET (fillfactor = 90); }
# Detach first so that decoding does not wait again at its next change.
step s2_detach	{ SELECT injection_points_detach('reorderbuffer-process-change'); }
step s2_wakeup	{ SELECT injection_points_wakeup('reorderbuffer-process-change'); }

permutation
	s2_prepare
	s1_decode
	s2_rollback_prepared
	s2_update_catalog
	s2_detach
	s2_wakeup
