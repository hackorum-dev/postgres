# This tests that the errors when data type conversion are correctly
# handled by logical replication apply workers
use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 3;

# Initialize publisher node
my $node_publisher = get_new_node('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = get_new_node('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

# Setup same table by different steps so that publisher and subscriber get
# different datatype OIDs.
$node_publisher->safe_psql('postgres', qq[
CREATE EXTENSION testsub2;
CREATE EXTENSION testsub1;
CREATE TABLE test (a dummytext, b dummyint);]);

$node_subscriber->safe_psql('postgres', qq[
CREATE EXTENSION testsub1;
CREATE EXTENSION testsub2;
CREATE TABLE test (b dummyint, a dummytext);]);

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres', qq[
CREATE PUBLICATION tap_pub FOR TABLE test
]);
my $appname = 'tap_sub';
$node_subscriber->safe_psql('postgres', qq[
CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub WITH (slot_name = tap_sub_slot, copy_data = false)
]);

# Truncate the logfile on subscriber before insertion in
# order to capture logs emitted by the callback function
# for the datatype conversion.
truncate $node_subscriber->logfile, 0;

# Insert test data, which will lead to call the callback function for the data
# type conversion on subscriber.
$node_publisher->safe_psql('postgres', qq(
INSERT INTO test VALUES ('one', '1');
));

$node_publisher->wait_for_catchup($appname);

# Check the callback function behavior for datatype conversion
# by checking the logs.
my $log = TestLib::slurp_file($node_subscriber->logfile);
like ($log,
	  qr/processing remote data for replication target relation "public.test" column "a", remote type public.dummytext, local type public.dummytext/,
	  'callbackfunction of datatype conversion1');
like ($log,
	  qr/processing remote data for replication target relation "public.test" column "b", remote type public.dummyint, local type public.dummyint/,
	  'callbackfunction of datatype conversion1');

# Check the data on subscriber
my $result = $node_subscriber->safe_psql('postgres', qq(
SELECT a FROM test;
));

# Inserted data is replicated correctly
is( $result, 'one');
