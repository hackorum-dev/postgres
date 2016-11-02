# Tests of privileges for logical replication
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 2;

my $node_publisher = get_new_node('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

my $node_subscriber = get_new_node('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

$node_publisher->safe_psql('postgres', qq(
CREATE USER test_user1;
CREATE USER test_user2 REPLICATION;
GRANT CREATE ON DATABASE postgres TO test_user1;

SET ROLE test_user1;
CREATE TABLE test1 (a int PRIMARY KEY, b text);
CREATE PUBLICATION mypub1 FOR TABLE test1;
));

my $appname = 'tap_sub';

$node_subscriber->safe_psql('postgres', qq(
CREATE TABLE test1 (a int PRIMARY KEY, b text);
CREATE SUBSCRIPTION mysub1 CONNECTION '$publisher_connstr user=test_user2 application_name=$appname' PUBLICATION mypub1;
));

$node_publisher->safe_psql('postgres', qq(
SET ROLE test_user1;
INSERT INTO test1 VALUES (1, 'one');
));

my $log = TestLib::slurp_file($node_publisher->logfile);
like($log, qr/permission denied for publication mypub1/, "permission denied on publication");

$node_publisher->safe_psql('postgres', qq(
SET ROLE test_user1;
GRANT USAGE ON PUBLICATION mypub1 TO test_user2;
));

# drop and recreate subscription so it sees the newly granted
# privileges
$node_subscriber->safe_psql('postgres', qq(
DROP SUBSCRIPTION mysub1;
CREATE SUBSCRIPTION mysub1 CONNECTION '$publisher_connstr user=test_user2 application_name=$appname' PUBLICATION mypub1;
));

$node_publisher->safe_psql('postgres', qq(
SET ROLE test_user1;
INSERT INTO test1 VALUES (2, 'two');
));

my $caughtup_query =
	"SELECT pg_current_wal_location() <= replay_location FROM pg_stat_replication WHERE application_name = '$appname';";
$node_publisher->poll_query_until('postgres', $caughtup_query)
	or die "Timed out while waiting for subscriber to catch up";

my $result = $node_subscriber->safe_psql('postgres', qq(
SELECT a, b FROM test1;
));

is($result, '2|two', 'replication catches up after privileges granted');

$node_subscriber->stop;
$node_publisher->stop;
