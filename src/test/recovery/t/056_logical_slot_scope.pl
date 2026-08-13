# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
my ($ret, $stdout, $stderr);
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', 'wal_level = replica');
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 10');
$node->start;

$node->safe_psql('postgres', q{
	CREATE TABLE orders (id integer PRIMARY KEY, payload text);
	CREATE TABLE customers (id integer PRIMARY KEY, payload text);
	CREATE PUBLICATION orders_pub FOR TABLE orders;
	CREATE PUBLICATION customers_pub FOR TABLE customers;
});

# Specifying publications should create a restricted logical slot.
like($node->safe_psql('postgres', q{
	SELECT slot_name FROM pg_create_logical_replication_slot(
		'orders_slot', 'pgoutput', publications => ARRAY['orders_pub']);
}), qr/orders_slot/, 'a publication list creates a restricted slot');

# pg_replication_slots should expose the durable restricted-scope state.
is($node->safe_psql('postgres', q{
	SELECT NOT unrestricted AND restricted_scope_ready AND
	       (SELECT oid FROM pg_publication WHERE pubname = 'orders_pub') =
	       ANY (publication_oids) AND restricted_scope_incarnation <> 0 AND
	       restricted_scope_ready_lsn IS NOT NULL
	FROM pg_replication_slots WHERE slot_name = 'orders_slot';
}), 't', 'restricted slot properties are exposed');

# Slot creation should install the initial relation mapping and flag.
is($node->safe_psql('postgres', q{
	SELECT relhasrestrictedslots AND EXISTS (
		SELECT 1 FROM pg_restricted_slot_relation
		WHERE rsrslotname = 'orders_slot' AND rsrrelid = 'orders'::regclass)
	FROM pg_class WHERE oid = 'orders'::regclass;
}), 't', 'initial publication membership is installed');

# A restricted slot should enable selective, rather than full, WAL.
is($node->safe_psql('postgres', q{
	SELECT current_setting('effective_wal_level'),
	       current_setting('restricted_wal_level');
}), "replica|logical", 'restricted slots enable only restricted logical WAL');

$node->stop('immediate');
$node->start;
# Scope readiness and selective WAL should survive an immediate restart.
is($node->safe_psql('postgres', q{
	SELECT restricted_scope_ready AND
	       current_setting('restricted_wal_level') = 'logical'
	FROM pg_replication_slots WHERE slot_name = 'orders_slot';
}), 't', 'scope readiness is durable across an immediate restart');

# NULL original arguments should preserve the function's former strictness.
is($node->safe_psql('postgres', q{
	SELECT pg_create_logical_replication_slot(NULL, 'pgoutput') IS NULL AND
	       pg_create_logical_replication_slot('null_plugin', NULL) IS NULL AND
	       pg_create_logical_replication_slot(
		   'null_temporary', 'pgoutput', NULL) IS NULL AND
	       pg_create_logical_replication_slot(
		   'null_twophase', 'pgoutput', false, NULL) IS NULL AND
	       pg_create_logical_replication_slot(
		   'null_failover', 'pgoutput', false, false, NULL) IS NULL;
}), 't', 'NULL original arguments retain strict behavior');

($ret, $stdout, $stderr) = $node->psql('postgres', q{
	SELECT pg_create_logical_replication_slot(
		'bad_plugin', 'test_decoding', publications => ARRAY['orders_pub']);
});
# Restricted slots should reject output plugins other than pgoutput.
isnt($ret, 0, 'a restricted slot requires pgoutput');
# The plugin restriction should produce the expected error message.
like($stderr, qr/restricted logical replication slots currently require pgoutput/,
	'wrong output plugin reports the restriction');

($ret, $stdout, $stderr) = $node->psql('postgres', q{
	BEGIN;
	SELECT pg_create_logical_replication_slot(
		'in_xact_slot', 'pgoutput', publications => ARRAY['orders_pub']);
});
# Restricted slot creation should be rejected in an explicit transaction.
isnt($ret, 0, 'restricted slot creation is rejected in a transaction block');
# Explicit-transaction rejection should report the expected error.
like($stderr, qr/restricted logical replication slots cannot be created inside a transaction block/,
	'explicit transaction reports the creation restriction');

($ret, $stdout, $stderr) = $node->psql('postgres', q{
	BEGIN;
	SAVEPOINT s;
	SELECT pg_create_logical_replication_slot(
		'in_subxact_slot', 'pgoutput', publications => ARRAY['orders_pub']);
});
# Restricted slot creation should also be rejected in a subtransaction.
isnt($ret, 0, 'restricted slot creation is rejected in a subtransaction');
# Subtransaction rejection should report the expected error.
like($stderr, qr/restricted logical replication slots cannot be created inside a transaction block/,
	'subtransaction reports the creation restriction');

($ret, $stdout, $stderr) = $node->psql('postgres', q{
	BEGIN;
	SELECT pg_copy_logical_replication_slot('orders_slot', 'copy_in_xact');
});
# Copying a restricted slot should be rejected in an explicit transaction.
isnt($ret, 0, 'restricted slot copying is rejected in a transaction block');
# Restricted-slot copy rejection should report the expected error.
like($stderr, qr/restricted logical replication slots cannot be copied inside a transaction block/,
	'explicit transaction reports the copy restriction');

($ret, $stdout, $stderr) = $node->psql('postgres', q{
	SELECT count(*) FROM pg_logical_slot_peek_binary_changes(
		'orders_slot', NULL, NULL,
		'proto_version', '1', 'publication_names', 'customers_pub');
});
# Decoding should reject a publication absent from the fixed set.
isnt($ret, 0, 'pgoutput rejects an unstored publication');
# Publication-set validation should produce the expected error.
like($stderr, qr/requested publications are not contained in replication slot/,
	'pgoutput reports publication-set containment failure');

($ret, $stdout, $stderr) = $node->psql('postgres', q{
	SELECT pg_create_logical_replication_slot(
		'null_publication_slot', 'pgoutput',
		publications => ARRAY['orders_pub', NULL]);
});
# A NULL publication name should be rejected as an invalid parameter.
isnt($ret, 0, 'NULL publication array element is rejected');
like($stderr, qr/publications must not contain null values/,
	'NULL publication array element reports the expected error');

$node->safe_psql('postgres', q{
	CREATE SUBSCRIPTION restricted_test_sub
	CONNECTION 'dbname=doesnotexist' PUBLICATION orders_pub, customers_pub
	WITH (connect = false, slot_name = NONE, unrestricted_slot = false);
});
($ret, $stdout, $stderr) = $node->psql('postgres', q{
	ALTER SUBSCRIPTION restricted_test_sub
	SET PUBLICATION outside_pub WITH (refresh = false);
});
# A restricted subscription should reject publication identity expansion.
isnt($ret, 0, 'restricted subscription publication expansion is rejected');
like($stderr,
	qr/cannot add publication "outside_pub" to restricted subscription "restricted_test_sub"/,
	'restricted subscription expansion reports the expected error');
$node->safe_psql('postgres', q{
	ALTER SUBSCRIPTION restricted_test_sub
	DROP PUBLICATION customers_pub WITH (refresh = false);
});
($ret, $stdout, $stderr) = $node->psql('postgres', q{
	ALTER SUBSCRIPTION restricted_test_sub
	ADD PUBLICATION customers_pub WITH (refresh = false);
});
# A removed publication should not be added back to a restricted subscription.
isnt($ret, 0, 'restricted subscription ADD PUBLICATION is rejected');
like($stderr,
	qr/cannot add publications to restricted subscription "restricted_test_sub"/,
	'restricted subscription ADD PUBLICATION reports the expected error');
$node->safe_psql('postgres', 'DROP SUBSCRIPTION restricted_test_sub');

$node->safe_psql('postgres', 'CREATE TABLE lock_target (id integer)');
my $writer = $node->background_psql('postgres');
$writer->query_safe('BEGIN');
$writer->query_safe('INSERT INTO lock_target VALUES (1)');
my $expander = $node->background_psql('postgres');
$expander->query_until(qr/alter-start/, q{
	\echo alter-start
	ALTER PUBLICATION orders_pub ADD TABLE lock_target;
	\echo alter-done
});
$node->poll_query_until('postgres', q{
	SELECT count(*) > 0 FROM pg_stat_activity
	WHERE query LIKE 'ALTER PUBLICATION orders_pub ADD TABLE lock_target%'
	  AND wait_event_type = 'Lock';
}, 't');
# Publication expansion should wait for an existing relation writer.
pass('publication expansion waits for concurrent relation writers');
$writer->query_safe('COMMIT');
$writer->quit;
$expander->quit;
# The relation flag should be installed after the writer releases its lock.
is($node->safe_psql('postgres', q{
	SELECT relhasrestrictedslots FROM pg_class
	WHERE oid = 'lock_target'::regclass;
}), 't', 'relation flag is visible after the writer interlock is released');

$node->safe_psql('postgres', 'ALTER PUBLICATION orders_pub ADD TABLE customers');
# Publication expansion should install mapping state synchronously.
is($node->safe_psql('postgres', q{
	SELECT relhasrestrictedslots AND EXISTS (
		SELECT 1 FROM pg_restricted_slot_relation
		WHERE rsrslotname = 'orders_slot' AND rsrrelid = 'customers'::regclass)
	FROM pg_class WHERE oid = 'customers'::regclass;
}), 't', 'publication expansion synchronously installs membership');

$node->safe_psql('postgres', q{
	CREATE TABLE unused_scope_table (id integer);
	CREATE PUBLICATION unused_scope_pub;
});
my $unused_scope_writer = $node->background_psql('postgres');
$unused_scope_writer->query_safe('BEGIN');
$unused_scope_writer->query_safe(
	'INSERT INTO unused_scope_table VALUES (1)');
# An unmatched publication should not take a stronger table lock.
is($node->safe_psql('postgres', q{
	SET statement_timeout = '1s';
	ALTER PUBLICATION unused_scope_pub ADD TABLE unused_scope_table;
	SELECT 1;
}), '1', 'unmatched publication expansion avoids closure locking');
$unused_scope_writer->query_safe('ROLLBACK');
$unused_scope_writer->quit;

$node->safe_psql('postgres', q{
	CREATE TABLE initializing_member (id integer);
	CREATE TABLE concurrent_member (id integer);
	CREATE PUBLICATION initializing_pub FOR TABLE initializing_member;
});
$node->safe_psql('postgres', q{
	SELECT pg_create_logical_replication_slot(
		'initializing_source', 'pgoutput',
		publications => ARRAY['initializing_pub']);
});
my $initialization_expander = $node->background_psql('postgres');
$initialization_expander->query_safe('BEGIN');
$initialization_expander->query_safe(
	'ALTER PUBLICATION initializing_pub ADD TABLE concurrent_member');
($ret, $stdout, $stderr) = $node->psql('postgres', q{
	SELECT pg_copy_logical_replication_slot(
		'initializing_source', 'initializing_slot');
});
# Copy initialization should fail instead of using a stale publication snapshot.
isnt($ret, 0, 'concurrent publication expansion rejects slot copying');
like($stderr,
	qr/could not initialize restricted logical replication slot due to concurrent activity/,
	'concurrent publication expansion reports the expected error');

$initialization_expander->query_safe('COMMIT');
$initialization_expander->quit;
$node->safe_psql('postgres', q{
	SELECT pg_copy_logical_replication_slot(
		'initializing_source', 'initializing_slot');
});

# Slot initialization should finish with the concurrent relation mapped.
is($node->safe_psql('postgres', q{
	SELECT s.restricted_scope_ready AND c.relhasrestrictedslots AND EXISTS (
		SELECT 1 FROM pg_restricted_slot_relation
		WHERE rsrslotname = 'initializing_slot'
		  AND rsrrelid = c.oid)
	FROM pg_replication_slots AS s, pg_class AS c
	WHERE s.slot_name = 'initializing_slot'
	  AND c.oid = 'concurrent_member'::regclass;
}), 't', 'initial scope retains a concurrent publication expansion');
$node->safe_psql('postgres', q{
	SELECT pg_drop_replication_slot('initializing_slot');
	SELECT pg_drop_replication_slot('initializing_source');
});

$node->safe_psql('postgres', q{
	CREATE TABLE lock_order_root (id integer) PARTITION BY RANGE (id);
	CREATE TABLE lock_order_child (LIKE lock_order_root);
	CREATE PUBLICATION lock_order_pub FOR TABLE lock_order_root;
});
$node->safe_psql('postgres', q{
	SELECT pg_create_logical_replication_slot(
		'lock_order_source', 'pgoutput',
		publications => ARRAY['lock_order_pub']);
});
$node->safe_psql('postgres', q{
	SELECT pg_create_logical_replication_slot(
		'lock_order_full_wal', 'pgoutput');
});
my $hierarchy_ddl =
  $node->background_psql('postgres', on_error_stop => 0);
$hierarchy_ddl->query_safe('BEGIN');
$hierarchy_ddl->query_safe(
	'LOCK TABLE lock_order_root IN ACCESS EXCLUSIVE MODE');

($ret, $stdout, $stderr) = $node->psql('postgres', q{
	SELECT pg_copy_logical_replication_slot(
		'lock_order_source', 'lock_order_slot');
});
# Slot copying should fail instead of waiting while holding the scope lock.
isnt($ret, 0,
	'restricted slot copying does not deadlock on a relation lock');
# The lock-order failure should identify concurrent scope initialization activity.
like($stderr,
	qr/could not initialize restricted logical replication slot due to concurrent activity/,
	'lock-order conflict reports the expected error');
$hierarchy_ddl->query_safe('ROLLBACK');
$hierarchy_ddl->quit;

# Retrying the slot copy should succeed after the relation lock is released.
$node->safe_psql('postgres', q{
	SELECT pg_copy_logical_replication_slot(
		'lock_order_source', 'lock_order_slot');
});
$node->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('lock_order_full_wal')");
$node->safe_psql('postgres', q{
	SELECT pg_drop_replication_slot('lock_order_slot');
	SELECT pg_drop_replication_slot('lock_order_source');
});

$node->safe_psql('postgres', q{
	CREATE TABLE command_scope_a (id integer);
	CREATE TABLE command_scope_b (id integer);
	CREATE PUBLICATION command_scope_pub_a;
	CREATE PUBLICATION command_scope_pub_b;
});
my $scope_xact_a = $node->background_psql('postgres');
my $scope_xact_b = $node->background_psql('postgres');
$scope_xact_a->query_safe('BEGIN');
$scope_xact_a->query_safe(
	'ALTER PUBLICATION command_scope_pub_a ADD TABLE command_scope_a');
$scope_xact_b->query_safe('BEGIN');
$scope_xact_b->query_safe(
	'ALTER PUBLICATION command_scope_pub_b ADD TABLE command_scope_b');

# A completed scope command should not retain the database scope lock.
pass('separate transactions can complete successive scope commands');
$scope_xact_a->query_safe('ROLLBACK');
$scope_xact_b->query_safe('ROLLBACK');
$scope_xact_a->quit;
$scope_xact_b->quit;

$node->safe_psql('postgres', q{
	CREATE SCHEMA published_schema;
	CREATE SCHEMA unpublished_schema;
	CREATE TABLE unpublished_schema.schema_move (id integer);
	CREATE PUBLICATION schema_move_pub
		FOR TABLES IN SCHEMA published_schema;
});
$node->safe_psql('postgres', q{
	SELECT pg_create_logical_replication_slot(
		'schema_move_slot', 'pgoutput',
		publications => ARRAY['schema_move_pub']);
});
$node->safe_psql('postgres', q{
	ALTER TABLE unpublished_schema.schema_move SET SCHEMA published_schema;
});

# SET SCHEMA should map a table moved into a published schema.
is($node->safe_psql('postgres', q{
	SELECT c.relhasrestrictedslots AND EXISTS (
		SELECT 1 FROM pg_restricted_slot_relation
		WHERE rsrslotname = 'schema_move_slot'
		  AND rsrrelid = c.oid)
	FROM pg_class AS c
	WHERE c.oid = 'published_schema.schema_move'::regclass;
}), 't', 'moving a table into a published schema installs its mapping');

$node->safe_psql('postgres', q{
	CREATE TABLE measurement (id integer, payload text) PARTITION BY RANGE (id);
	CREATE TABLE measurement_old PARTITION OF measurement FOR VALUES FROM (0) TO (10);
	ALTER PUBLICATION orders_pub ADD TABLE measurement;
	CREATE TABLE measurement_new (LIKE measurement);
	ALTER TABLE measurement ATTACH PARTITION measurement_new FOR VALUES FROM (10) TO (20);
});
# Attaching a partition should propagate its parent's restricted mapping.
is($node->safe_psql('postgres', q{
	SELECT relhasrestrictedslots AND EXISTS (
		SELECT 1 FROM pg_restricted_slot_relation
		WHERE rsrslotname = 'orders_slot' AND rsrrelid = 'measurement_new'::regclass)
	FROM pg_class WHERE oid = 'measurement_new'::regclass;
}), 't', 'partition attachment propagates restricted membership');

$node->safe_psql('postgres', q{
	CREATE TABLE root_mode (id integer, payload text) PARTITION BY RANGE (id);
	CREATE TABLE root_mode_old PARTITION OF root_mode
		FOR VALUES FROM (0) TO (10);
	CREATE PUBLICATION root_mode_pub FOR TABLE root_mode
		WITH (publish_via_partition_root = true);
});
$node->safe_psql('postgres', q{
	SELECT pg_create_logical_replication_slot(
		'root_mode_slot', 'pgoutput', publications => ARRAY['root_mode_pub']);
});
$node->safe_psql('postgres', q{
	CREATE TABLE root_mode_new (LIKE root_mode);
	BEGIN;
	ALTER TABLE root_mode ATTACH PARTITION root_mode_new
		FOR VALUES FROM (10) TO (20);
	INSERT INTO root_mode VALUES (10, 'new partition');
	COMMIT;
});
# The root, old leaf, and newly attached leaf should all be mapped.
is($node->safe_psql('postgres', q{
	SELECT bool_and(c.relhasrestrictedslots) AND count(*) = 3
	FROM pg_class AS c
	JOIN pg_restricted_slot_relation AS m ON m.rsrrelid = c.oid
	WHERE m.rsrslotname = 'root_mode_slot'
	  AND c.oid IN ('root_mode'::regclass,
				  'root_mode_old'::regclass,
				  'root_mode_new'::regclass);
}), 't', 'partition-root publication maps a newly attached partition');

# Table synchronization should handle an attached partition with both settings
# of publish_via_partition_root.  In leaf mode, REFRESH PUBLICATION discovers
# a new subscription relation and copies its existing data.  In root mode, the
# root is already a subscription relation, so refresh does not initiate another
# table synchronization for the newly attached leaf.
my $subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$subscriber->init(allows_streaming => 'logical');
$subscriber->start;
my $publisher_connstr = $node->connstr . ' dbname=postgres';

$node->safe_psql('postgres', q{
	CREATE TABLE leaf_copy_root (id integer, payload text)
		PARTITION BY RANGE (id);
	CREATE TABLE leaf_copy_old PARTITION OF leaf_copy_root
		FOR VALUES FROM (0) TO (10);
	CREATE TABLE leaf_copy_new (LIKE leaf_copy_root);
	INSERT INTO leaf_copy_new VALUES (10, 'before attach');
	CREATE PUBLICATION leaf_copy_pub FOR TABLE leaf_copy_root
		WITH (publish_via_partition_root = false);

	CREATE TABLE root_copy_root (id integer, payload text)
		PARTITION BY RANGE (id);
	CREATE TABLE root_copy_old PARTITION OF root_copy_root
		FOR VALUES FROM (0) TO (10);
	CREATE TABLE root_copy_new (LIKE root_copy_root);
	INSERT INTO root_copy_new VALUES (10, 'before attach');
	CREATE PUBLICATION root_copy_pub FOR TABLE root_copy_root
		WITH (publish_via_partition_root = true);
});
$subscriber->safe_psql('postgres', q{
	CREATE TABLE leaf_copy_root (id integer, payload text)
		PARTITION BY RANGE (id);
	CREATE TABLE leaf_copy_old PARTITION OF leaf_copy_root
		FOR VALUES FROM (0) TO (10);
	CREATE TABLE leaf_copy_new PARTITION OF leaf_copy_root
		FOR VALUES FROM (10) TO (20);

	CREATE TABLE root_copy_root (id integer, payload text)
		PARTITION BY RANGE (id);
	CREATE TABLE root_copy_old PARTITION OF root_copy_root
		FOR VALUES FROM (0) TO (10);
	CREATE TABLE root_copy_new PARTITION OF root_copy_root
		FOR VALUES FROM (10) TO (20);
});
$subscriber->safe_psql('postgres', qq{
	CREATE SUBSCRIPTION leaf_copy_sub CONNECTION '$publisher_connstr'
		PUBLICATION leaf_copy_pub
		WITH (copy_data = true, unrestricted_slot = false);
	CREATE SUBSCRIPTION root_copy_sub CONNECTION '$publisher_connstr'
		PUBLICATION root_copy_pub
		WITH (copy_data = true, unrestricted_slot = false);
});
$subscriber->wait_for_subscription_sync($node, 'leaf_copy_sub');
$subscriber->wait_for_subscription_sync($node, 'root_copy_sub');

$node->safe_psql('postgres', q{
	ALTER TABLE leaf_copy_root ATTACH PARTITION leaf_copy_new
		FOR VALUES FROM (10) TO (20);
	ALTER TABLE root_copy_root ATTACH PARTITION root_copy_new
		FOR VALUES FROM (10) TO (20);
});
$subscriber->safe_psql('postgres', q{
	ALTER SUBSCRIPTION leaf_copy_sub REFRESH PUBLICATION
		WITH (copy_data = true);
	ALTER SUBSCRIPTION root_copy_sub REFRESH PUBLICATION
		WITH (copy_data = true);
});
$subscriber->wait_for_subscription_sync($node, 'leaf_copy_sub');
$subscriber->wait_for_subscription_sync($node, 'root_copy_sub');

# Leaf-mode refresh should copy data that predates partition attachment.
is($subscriber->safe_psql('postgres', q{
	SELECT string_agg(payload, ',' ORDER BY id) FROM leaf_copy_root;
}), 'before attach',
	'leaf publication copies a row that existed before partition attachment');
# Root-mode refresh should not resynchronize an already known root table.
is($subscriber->safe_psql('postgres', q{
	SELECT count(*) FROM root_copy_root;
}), '0',
	'partition-root publication does not recopy its existing root relation');

$node->safe_psql('postgres', q{
	INSERT INTO leaf_copy_root VALUES (11, 'after attach');
	INSERT INTO root_copy_root VALUES (11, 'after attach');
});
$node->wait_for_catchup('leaf_copy_sub');
$node->wait_for_catchup('root_copy_sub');

# Leaf mode should stream changes made after partition attachment.
is($subscriber->safe_psql('postgres', q{
	SELECT string_agg(payload, ',' ORDER BY id) FROM leaf_copy_root;
}), 'before attach,after attach',
	'leaf publication streams rows inserted after partition attachment');
# Root mode should stream changes made after partition attachment.
is($subscriber->safe_psql('postgres', q{
	SELECT string_agg(payload, ',' ORDER BY id) FROM root_copy_root;
}), 'after attach',
	'partition-root publication streams rows inserted after attachment');

$subscriber->safe_psql('postgres',
	'ALTER SUBSCRIPTION root_copy_sub DISABLE');
$node->poll_query_until('postgres', q{
	SELECT NOT active FROM pg_replication_slots
	WHERE slot_name = 'root_copy_sub';
}, 't');
$subscriber->safe_psql('postgres', q{
	CREATE TABLE root_history_part PARTITION OF root_copy_root
		FOR VALUES FROM (20) TO (30);
});
$node->safe_psql('postgres', q{
	CREATE TABLE root_history_part (LIKE root_copy_root);
	SELECT pg_create_logical_replication_slot(
		'history_cover_slot', 'test_decoding');
	INSERT INTO root_history_part VALUES (20, 'existing before attach');
	INSERT INTO root_history_part VALUES (21, 'before attach');
	ALTER TABLE root_copy_root ATTACH PARTITION root_history_part
		FOR VALUES FROM (20) TO (30);
	INSERT INTO root_history_part VALUES (22, 'after attach');
	ALTER TABLE root_copy_root DETACH PARTITION root_history_part;
	INSERT INTO root_history_part VALUES (23, 'after detach');
	ALTER TABLE root_copy_root ATTACH PARTITION root_history_part
		FOR VALUES FROM (20) TO (30);
	INSERT INTO root_history_part VALUES (24, 'after reattach');
});
$subscriber->safe_psql('postgres',
	'ALTER SUBSCRIPTION root_copy_sub ENABLE');
$node->wait_for_catchup('root_copy_sub');

# Delayed decoding should use partition membership at each change's WAL point.
is($subscriber->safe_psql('postgres', q{
	SELECT string_agg(id || ':' || payload, ',' ORDER BY id)
	FROM root_copy_root WHERE id >= 20;
}), '22:after attach,24:after reattach',
	'delayed decoding follows attach and detach boundaries');

$node->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('history_cover_slot')");

$subscriber->safe_psql('postgres',
	'ALTER SUBSCRIPTION leaf_copy_sub DISABLE');
$node->safe_psql('postgres',
	q{INSERT INTO leaf_copy_root VALUES (12, 'while disabled')});

# A disabled subscription should not apply newly generated changes.
is($subscriber->safe_psql('postgres', q{
	SELECT string_agg(payload, ',' ORDER BY id) FROM leaf_copy_root;
}), 'before attach,after attach',
	'a disabled subscription does not apply new changes');

$subscriber->safe_psql('postgres',
	'ALTER SUBSCRIPTION leaf_copy_sub ENABLE');
$node->wait_for_catchup('leaf_copy_sub');

# Enabling a subscription should replay scoped WAL written while disabled.
is($subscriber->safe_psql('postgres', q{
	SELECT string_agg(payload, ',' ORDER BY id) FROM leaf_copy_root;
}), 'before attach,after attach,while disabled',
	'enabling a subscription replays changes generated while disabled');

$node->safe_psql('postgres', q{
	CREATE TABLE speculative_unscoped (
		id integer PRIMARY KEY, payload text);
	BEGIN;
	INSERT INTO speculative_unscoped VALUES (1, repeat('x', 100000))
		ON CONFLICT DO NOTHING;
	INSERT INTO leaf_copy_root VALUES (13, 'after speculative insert');
	COMMIT;
});
$node->wait_for_catchup('leaf_copy_sub');

# An unscoped speculative insert should not prevent decoding a scoped change.
is($subscriber->safe_psql('postgres', q{
	SELECT payload FROM leaf_copy_root WHERE id = 13;
}), 'after speculative insert',
	'decoding preserves unscoped speculative insertion ordering');

$node->safe_psql('postgres', q{
	ALTER TABLE measurement DROP COLUMN payload;
	CREATE TABLE measurement_after_drop (LIKE measurement);
	ALTER TABLE measurement ATTACH PARTITION measurement_after_drop DEFAULT;
});
# Dropped columns should not lose mappings needed by later partitions.
is($node->safe_psql('postgres', q{
	SELECT EXISTS (
		SELECT 1 FROM pg_restricted_slot_relation
		WHERE rsrslotname = 'orders_slot'
		  AND rsrrelid = 'measurement_after_drop'::regclass);
}), 't', 'dropping a column preserves mappings used by later attachment');

$node->safe_psql('postgres', q{
	CREATE TABLE toast_later (id integer);
	ALTER PUBLICATION orders_pub ADD TABLE toast_later;
	BEGIN;
	ALTER TABLE toast_later ADD COLUMN payload text;
	INSERT INTO toast_later VALUES (1, repeat('x', 100000));
	COMMIT;
});
# TOAST creation should propagate mapping before same-transaction DML.
is($node->safe_psql('postgres', q{
	SELECT toast.relhasrestrictedslots AND EXISTS (
		SELECT 1 FROM pg_restricted_slot_relation
		WHERE rsrslotname = 'orders_slot' AND rsrrelid = owner.reltoastrelid)
	FROM pg_class owner JOIN pg_class toast ON toast.oid = owner.reltoastrelid
	WHERE owner.oid = 'toast_later'::regclass;
}), 't', 'TOAST creation propagates membership before same-transaction DML');

$node->restart;
# Restricted metadata and selective WAL should remain active after restart.
is($node->safe_psql('postgres', q{
	SELECT NOT unrestricted AND restricted_scope_ready AND
	       current_setting('restricted_wal_level') = 'logical'
	FROM pg_replication_slots WHERE slot_name = 'orders_slot';
}), 't', 'restricted slot metadata and WAL level survive restart');

$node->safe_psql('postgres', q{
	SELECT pg_create_logical_replication_slot('general_slot', 'test_decoding');
});
# Omitting publications should retain ordinary unrestricted-slot behavior.
is($node->safe_psql('postgres', q{
	SELECT unrestricted AND publication_oids IS NULL
	FROM pg_replication_slots WHERE slot_name = 'general_slot';
}), 't', 'an omitted publication list creates an unrestricted slot');

$node->safe_psql('postgres', q{
	SELECT pg_create_logical_replication_slot(
		'reuse_slot', 'pgoutput', publications => ARRAY['orders_pub']);
});
my $old_incarnation = $node->safe_psql('postgres', q{
	SELECT restricted_scope_incarnation FROM pg_replication_slots
	WHERE slot_name = 'reuse_slot';
});
$node->safe_psql('postgres', q{
	SELECT pg_drop_replication_slot('reuse_slot');
	SELECT pg_create_logical_replication_slot(
		'reuse_slot', 'pgoutput', publications => ARRAY['orders_pub']);
});
my $new_incarnation = $node->safe_psql('postgres', q{
	SELECT restricted_scope_incarnation FROM pg_replication_slots
	WHERE slot_name = 'reuse_slot';
});
# A recreated slot name should not reuse the old scope incarnation.
isnt($new_incarnation, $old_incarnation,
	'slot name reuse receives a distinct durable incarnation');

done_testing();
