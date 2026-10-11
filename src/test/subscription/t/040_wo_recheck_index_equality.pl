# Copyright (c) 2026, PostgreSQL Global Development Group

# Test with a lossy REPLICA IDENTITY index whose equality operator differs
# from the key type's default equality
#
# test_lossy_gist_ops provides a lossy GiST opclass for text whose equality
# (===) is case-insensitive, unlike the type's default equality.  The
# subscriber's row is found through the index because the operator class
# considers 'abc' and 'ABC' equal.  If the apply worker rechecked the
# candidate with the type's default equality instead of the index's, it would
# reject the row and lose the update.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

if ($node_subscriber->check_extension('test_lossy_gist_ops') == 0)
{
	plan skip_all => 'test_lossy_gist_ops extension not available';
}

# Publisher: simple table; REPLICA IDENTITY FULL so that the whole old row is
# sent as the key.
$node_publisher->safe_psql(
	'postgres', "
	CREATE TABLE wo_ci (k text, mr int4multirange, v int);
	ALTER TABLE wo_ci REPLICA IDENTITY FULL;
	INSERT INTO wo_ci VALUES ('abc', '{[1,5)}', 1);
	CREATE PUBLICATION pub FOR TABLE wo_ci;
");

# Subscriber: a WITHOUT OVERLAPS primary key over (k, mr), which picks up the
# lossy opclass for k.  The local row matches the publisher's key only under
# the index's equality.
$node_subscriber->safe_psql(
	'postgres', "
	CREATE EXTENSION test_lossy_gist_ops;
	CREATE TABLE wo_ci (k text NOT NULL, mr int4multirange NOT NULL, v int,
						PRIMARY KEY (k, mr WITHOUT OVERLAPS));
	INSERT INTO wo_ci VALUES ('ABC', '{[1,5)}', 0);
");

# The opclass equality is case-insensitive, the type default is not.
is( $node_subscriber->safe_psql('postgres',
		"SELECT 'abc' === 'ABC', 'abc' = 'ABC'"),
	't|f',
	'opclass equality is case-insensitive, type equality is not');

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub CONNECTION '$publisher_connstr' PUBLICATION pub WITH (copy_data = false)"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'sub');

my $log_offset = -s $node_subscriber->logfile;

# The publisher updates its row; the old key sent is 'abc'.
$node_publisher->safe_psql('postgres',
	"UPDATE wo_ci SET v = 2 WHERE k = 'abc'");
$node_publisher->wait_for_catchup('sub');

# The update must have been applied.  The key now carries the publisher's
# spelling (REPLICA IDENTITY FULL sends the whole new row), so check by value.
is( $node_subscriber->safe_psql('postgres', "SELECT v FROM wo_ci"),
	'2',
	'update applied to the row the identity index considers a match');

my $logfile = slurp_file($node_subscriber->logfile(), $log_offset);
ok( $logfile !~ /conflict detected on relation "public.wo_ci"/,
	'no spurious update_missing conflict');

done_testing();
