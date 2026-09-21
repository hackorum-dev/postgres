
# Copyright (c) 2026, PostgreSQL Global Development Group

# Logical replication tests for publications with EXCEPT clause
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

# Initialize subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

my $result;

sub test_except_root_partition
{
	my ($pubviaroot) = @_;

	# If the root partitioned table is in the EXCEPT clause, all its
	# partitions are excluded from publication, regardless of the
	# publish_via_partition_root setting.
	$node_publisher->safe_psql(
		'postgres', qq(
		CREATE PUBLICATION tap_pub_part FOR ALL TABLES EXCEPT (TABLE root1) WITH (publish_via_partition_root = $pubviaroot);
		INSERT INTO root1 VALUES (1), (101);
	));
	$node_subscriber->safe_psql('postgres',
		"CREATE SUBSCRIPTION tap_sub_part CONNECTION '$publisher_connstr' PUBLICATION tap_pub_part"
	);
	$node_subscriber->wait_for_subscription_sync($node_publisher,
		'tap_sub_part');

	# Advance the replication slot to ignore changes generated before this point.
	$node_publisher->safe_psql('postgres',
		"SELECT slot_name FROM pg_replication_slot_advance('test_slot', pg_current_wal_lsn())"
	);
	$node_publisher->safe_psql('postgres',
		"INSERT INTO root1 VALUES (2), (102)");

	# Verify that data inserted into the partitioned table is not published when
	# it is in the EXCEPT clause.
	$result = $node_publisher->safe_psql('postgres',
		"SELECT count(*) = 0 FROM pg_logical_slot_get_binary_changes('test_slot', NULL, NULL, 'proto_version', '1', 'publication_names', 'tap_pub_part')"
	);
	is($result, qq(t),
		"no changes for the partitioned table in the EXCEPT clause are present in the replication slot (publish_via_partition_root = $pubviaroot)"
	);

	$node_publisher->wait_for_catchup('tap_sub_part');

	# Verify that no rows are replicated to subscriber for root or partitions.
	foreach my $table (qw(root1 part1 part2 part2_1))
	{
		$result = $node_subscriber->safe_psql('postgres',
			"SELECT count(*) FROM $table");
		is($result, qq(0), "no rows replicated to subscriber for $table");
	}

	$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_part");
	$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_part");
}

# ============================================
# EXCEPT clause test cases for non-partitioned tables and inherited tables.
# ============================================

# Create tables on publisher
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab1 AS SELECT generate_series(1,10) AS a;
	CREATE TABLE parent (a int);
	CREATE TABLE child (b int) INHERITS (parent);
	CREATE TABLE parent1 (a int);
	CREATE TABLE child1 (b int) INHERITS (parent1);
));

# Create tables on subscriber
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab1 (a int);
	CREATE TABLE parent (a int);
	CREATE TABLE child (b int) INHERITS (parent);
	CREATE TABLE parent1 (a int);
	CREATE TABLE child1 (b int) INHERITS (parent1);
));

# Exclude tab1 (non-inheritance case), and also exclude parent and ONLY parent1
# to verify exclusion behavior for inherited tables, including the effect of
# ONLY in the EXCEPT clause.
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR ALL TABLES EXCEPT (TABLE tab1, parent, only parent1)"
);

# Create a logical replication slot to help with later tests.
$node_publisher->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('test_slot', 'pgoutput')");

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub"
);

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

# Check the table data does not sync for the tables specified in the EXCEPT
# clause.
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab1");
is($result, qq(0),
	'check there is no initial data copied for the tables specified in the EXCEPT clause'
);

# Insert some data into the table listed in the EXCEPT clause
$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab1 VALUES(generate_series(11,20));
	INSERT INTO child VALUES(generate_series(11,20), generate_series(11,20));
));

# Verify that data inserted into a table listed in the EXCEPT clause is
# not published.
$result = $node_publisher->safe_psql('postgres',
	"SELECT count(*) = 0 FROM pg_logical_slot_get_binary_changes('test_slot', NULL, NULL, 'proto_version', '1', 'publication_names', 'tap_pub')"
);
is($result, qq(t),
	'verify no changes for table listed in the EXCEPT clause are present in the replication slot'
);

# This should be published because ONLY parent1 was specified in the
# EXCEPT clause, so the exclusion applies only to the parent table and not
# to its child.
$node_publisher->safe_psql('postgres',
	"INSERT INTO child1 VALUES(generate_series(11,20), generate_series(11,20))"
);

# Verify that data inserted into a table listed in the EXCEPT clause is
# not replicated.
$node_publisher->wait_for_catchup('tap_sub');
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab1");
is($result, qq(0), 'check replicated inserts on subscriber');
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM child");
is($result, qq(0), 'check replicated inserts on subscriber');
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM child1");
is($result, qq(10), 'check replicated inserts on subscriber');

$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab2 AS SELECT generate_series(1,10) AS a");
$node_subscriber->safe_psql('postgres', "CREATE TABLE tab2 (a int)");

# Replace the table list in the EXCEPT clause so that only tab2 is excluded.
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub SET ALL TABLES EXCEPT (TABLE tab2)");

# Refresh the subscription so the subscriber picks up the updated
# publication definition and initiates table synchronization.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub REFRESH PUBLICATION");

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

# Verify that initial table synchronization does not occur for tables
# listed in the EXCEPT clause.
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab2");
is($result, qq(0),
	'check there is no initial data copied for the tables specified in the EXCEPT clause'
);

# Verify that table synchronization now happens for tab1. Table tab1 is
# included now since the table list of EXCEPT clause is only (tab2).
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab1");
is($result, qq(20),
	'check that the data is copied as the tab1 is removed from EXCEPT clause'
);

# cleanup
$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION tap_sub;
	TRUNCATE TABLE tab1;
	DROP TABLE parent, parent1, child, child1, tab2;
));
$node_publisher->safe_psql(
	'postgres', qq(
	DROP PUBLICATION tap_pub;
	TRUNCATE TABLE tab1;
    DROP TABLE parent, parent1, child, child1, tab2;
));

# ============================================
# EXCEPT clause test cases for partitioned tables
# ============================================
# Setup partitioned table and partitions on the publisher that map to normal
# tables on the subscriber.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE root1(a int) PARTITION BY RANGE(a);
	CREATE TABLE part1 PARTITION OF root1 FOR VALUES FROM (0) TO (100);
	CREATE TABLE part2 PARTITION OF root1 FOR VALUES FROM (100) TO (200) PARTITION BY RANGE(a);
	CREATE TABLE part2_1 PARTITION OF part2 FOR VALUES FROM (100) TO (150);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE root1(a int);
	CREATE TABLE part1(a int);
	CREATE TABLE part2(a int);
	CREATE TABLE part2_1(a int);
));

# Validate the behaviour with both publish_via_partition_root as true and false
test_except_root_partition('false');
test_except_root_partition('true');

# ============================================
# Test when a subscription is subscribing to multiple publications
# ============================================

# OK when a table is excluded by pub1 EXCEPT clause, but it is included by pub2
# FOR TABLE.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE PUBLICATION tap_pub1 FOR ALL TABLES EXCEPT (TABLE tab1);
	CREATE PUBLICATION tap_pub2 FOR TABLE tab1;
	INSERT INTO tab1 VALUES(1);
));
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub1, tap_pub2"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

$node_publisher->safe_psql('postgres', qq(INSERT INTO tab1 VALUES(2)));
$node_publisher->wait_for_catchup('tap_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT * FROM tab1 ORDER BY a");
is( $result, qq(1
2),
	"check replication of a table in the EXCEPT clause of one publication but included by another"
);

$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION tap_sub;
	TRUNCATE tab1;
));
$node_publisher->safe_psql(
	'postgres', qq(
	DROP PUBLICATION tap_pub2;
	TRUNCATE tab1;
));

# OK when a table is excluded by pub1 EXCEPT clause, but it is included by pub2
# FOR ALL TABLES.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE PUBLICATION tap_pub2 FOR ALL TABLES;
	INSERT INTO tab1 VALUES(1);
));
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub1, tap_pub2"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

$node_publisher->safe_psql('postgres', qq(INSERT INTO tab1 VALUES(2)));
$node_publisher->wait_for_catchup('tap_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT * FROM tab1 ORDER BY a");
is( $result, qq(1
2),
	"check replication of a table in the EXCEPT clause of one publication but included by another"
);

# ============================================
# Partition whose concurrent detach has not been finalized
# ============================================
# ALTER TABLE ... DETACH PARTITION ... CONCURRENTLY leaves the partition
# detach-pending when it is interrupted while waiting for lockers.  In that
# state relispartition is still set but the partition has no ancestors, and
# decoding its changes for a FOR ALL TABLES publication must not look for a
# top-most ancestor to evaluate the EXCEPT clause on.
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub");
$node_publisher->safe_psql(
	'postgres', qq(
	DROP PUBLICATION tap_pub1, tap_pub2;
	CREATE TABLE tab_detach (a int PRIMARY KEY) PARTITION BY LIST (a);
	CREATE TABLE tab_detach1 PARTITION OF tab_detach FOR VALUES IN (1);
	INSERT INTO tab_detach VALUES (1);
));
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_detach1 (a int PRIMARY KEY)");

# Hold a lock on the partition so the concurrent detach blocks after its
# first transaction has committed, then cancel it while it waits.
my $lock_session = $node_publisher->background_psql('postgres');
$lock_session->query_safe("BEGIN; SELECT * FROM tab_detach;");

my $detach_session =
  $node_publisher->background_psql('postgres', on_error_stop => 0);
my $detach_pid = $detach_session->query('SELECT pg_backend_pid()');
$detach_session->query_until(qr//,
	"ALTER TABLE tab_detach DETACH PARTITION tab_detach1 CONCURRENTLY;\n");

$node_publisher->poll_query_until('postgres',
	"SELECT wait_event_type = 'Lock' FROM pg_stat_activity WHERE pid = $detach_pid"
) or die "timed out waiting for the concurrent detach to block";

$node_publisher->safe_psql('postgres',
	"SELECT pg_cancel_backend($detach_pid)");
ok( pump_until(
		$detach_session->{run}, $detach_session->{timeout},
		\$detach_session->{stderr},
		qr/canceling statement due to user request/),
	'concurrent detach canceled');
$detach_session->quit;
$lock_session->query_safe("COMMIT");
$lock_session->quit;

$result = $node_publisher->safe_psql(
	'postgres', qq(
	SELECT c.relispartition, i.inhdetachpending
	FROM pg_class c JOIN pg_inherits i ON i.inhrelid = c.oid
	WHERE c.oid = 'tab_detach1'::regclass));
is($result, qq(t|t), 'partition is left detach-pending');

# Decode an update of the detach-pending partition for FOR ALL TABLES
# publications, with and without publish_via_partition_root.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE PUBLICATION tap_pub_detach FOR ALL TABLES;
	CREATE PUBLICATION tap_pub_detach_viaroot FOR ALL TABLES
		WITH (publish_via_partition_root = true);
	SELECT pg_replication_slot_advance('test_slot', pg_current_wal_lsn());
	UPDATE tab_detach1 SET a = 1;
));

foreach my $pub (qw(tap_pub_detach tap_pub_detach_viaroot))
{
	$result = $node_publisher->safe_psql('postgres',
		"SELECT count(*) > 0 FROM pg_logical_slot_peek_binary_changes('test_slot', NULL, NULL, 'proto_version', '1', 'publication_names', '$pub')"
	);
	is($result, qq(t),
		"changes of a detach-pending partition are decoded for $pub");
}

# The detach-pending partition is published like a standalone table.
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub_detach"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

$node_publisher->safe_psql('postgres', "UPDATE tab_detach1 SET a = 1");
$node_publisher->wait_for_catchup('tap_sub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM tab_detach1");
is($result, qq(1),
	'detach-pending partition is replicated as a standalone table');

done_testing();
