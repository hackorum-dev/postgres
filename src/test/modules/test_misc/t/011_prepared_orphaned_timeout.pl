
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test for prepared_orphaned_transaction_timeout GUC.
# Verifies that orphaned prepared transactions are automatically
# rolled back by the checkpointer when they exceed the configured timeout.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

#
# Test 1: Basic orphaned prepared transaction cleanup
#
# Set up a node with prepared transactions enabled and a short timeout.
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
	max_prepared_transactions = 5
	prepared_orphaned_transaction_timeout = '2s'
	log_min_messages = log
));
$node->start;

# Create a test table and prepare a transaction.
$node->safe_psql('postgres', 'CREATE TABLE t_orphan_test (id int, msg text)');
$node->safe_psql(
	'postgres', "
	BEGIN;
	INSERT INTO t_orphan_test VALUES (1, 'orphaned');
	PREPARE TRANSACTION 'orphan_xact_1';
");

# Verify the prepared transaction exists.
my $result = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts WHERE gid = 'orphan_xact_1'");
is($result, '1', 'prepared transaction orphan_xact_1 exists');

# Record log position before the timeout fires.
my $log_offset = -s $node->logfile;

# Wait for the timeout to elapse, then trigger the checkpointer loop
# by issuing a CHECKPOINT. The checkpointer will scan for orphaned
# prepared transactions as part of its main loop iteration.
sleep(3);
$node->safe_psql('postgres', 'CHECKPOINT');

# Give the checkpointer a moment to process the cleanup.
sleep(1);

# Verify the prepared transaction was rolled back.
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) FROM pg_prepared_xacts WHERE gid = 'orphan_xact_1'",
		'0'),
	'orphaned prepared transaction was rolled back');

# Verify that the inserted data was NOT committed (rolled back).
$result = $node->safe_psql('postgres',
	"SELECT count(*) FROM t_orphan_test WHERE id = 1");
is($result, '0', 'orphaned transaction data was not committed');

# Verify the rollback was logged.
$node->wait_for_log(
	qr/rolling back orphaned prepared transaction "orphan_xact_1"/,
	$log_offset);
ok(1, 'orphaned transaction rollback was logged');


#
# Test 2: Prepared transaction committed before timeout is not affected
#
$node->safe_psql(
	'postgres', "
	BEGIN;
	INSERT INTO t_orphan_test VALUES (2, 'committed_in_time');
	PREPARE TRANSACTION 'timely_xact';
");

# Commit the prepared transaction immediately (before timeout).
$node->safe_psql('postgres', "COMMIT PREPARED 'timely_xact'");

# Verify the data was committed.
$result = $node->safe_psql('postgres',
	"SELECT count(*) FROM t_orphan_test WHERE id = 2");
is($result, '1',
	'prepared transaction committed before timeout preserved data');


#
# Test 3: Timeout of 0 disables the feature
#
$node->safe_psql('postgres',
	"ALTER SYSTEM SET prepared_orphaned_transaction_timeout = '0'");
$node->safe_psql('postgres', "SELECT pg_reload_conf()");

$node->safe_psql(
	'postgres', "
	BEGIN;
	INSERT INTO t_orphan_test VALUES (3, 'should_persist');
	PREPARE TRANSACTION 'persist_xact';
");

# Wait and trigger checkpointer - the transaction should NOT be rolled back.
sleep(3);
$node->safe_psql('postgres', 'CHECKPOINT');
sleep(1);

$result = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts WHERE gid = 'persist_xact'");
is($result, '1',
	'prepared transaction not rolled back when timeout is disabled');

# Clean up.
$node->safe_psql('postgres', "ROLLBACK PREPARED 'persist_xact'");


#
# Test 4: Multiple orphaned transactions are all cleaned up
#
$node->safe_psql('postgres',
	"ALTER SYSTEM SET prepared_orphaned_transaction_timeout = '2s'");
$node->safe_psql('postgres', "SELECT pg_reload_conf()");

$node->safe_psql(
	'postgres', "
	BEGIN;
	INSERT INTO t_orphan_test VALUES (10, 'multi_1');
	PREPARE TRANSACTION 'multi_orphan_1';
");

$node->safe_psql(
	'postgres', "
	BEGIN;
	INSERT INTO t_orphan_test VALUES (11, 'multi_2');
	PREPARE TRANSACTION 'multi_orphan_2';
");

$node->safe_psql(
	'postgres', "
	BEGIN;
	INSERT INTO t_orphan_test VALUES (12, 'multi_3');
	PREPARE TRANSACTION 'multi_orphan_3';
");

$result = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts");
is($result, '3', 'three prepared transactions exist');

# Wait for timeout, trigger checkpointer.
sleep(3);
$node->safe_psql('postgres', 'CHECKPOINT');

# All three should be cleaned up.
ok( $node->poll_query_until(
		'postgres', "SELECT count(*) FROM pg_prepared_xacts", '0'),
	'all orphaned prepared transactions were rolled back');


#
# Test 5: Timeout change via reload takes effect
#
# Set a very long timeout so nothing gets cleaned.
$node->safe_psql('postgres',
	"ALTER SYSTEM SET prepared_orphaned_transaction_timeout = '1h'");
$node->safe_psql('postgres', "SELECT pg_reload_conf()");

$node->safe_psql(
	'postgres', "
	BEGIN;
	INSERT INTO t_orphan_test VALUES (20, 'reload_test');
	PREPARE TRANSACTION 'reload_xact';
");

sleep(3);
$node->safe_psql('postgres', 'CHECKPOINT');
sleep(1);

$result = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts WHERE gid = 'reload_xact'");
is($result, '1',
	'prepared transaction persists with long timeout');

# Now lower the timeout and reload.
$node->safe_psql('postgres',
	"ALTER SYSTEM SET prepared_orphaned_transaction_timeout = '1s'");
$node->safe_psql('postgres', "SELECT pg_reload_conf()");

sleep(2);
$node->safe_psql('postgres', 'CHECKPOINT');

ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) FROM pg_prepared_xacts WHERE gid = 'reload_xact'",
		'0'),
	'prepared transaction cleaned up after lowering timeout via reload');

$node->stop;
done_testing();
