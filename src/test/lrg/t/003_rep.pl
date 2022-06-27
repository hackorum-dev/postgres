# Copyright (c) 2022, PostgreSQL Global Development Group

#
# Basic LRG tests: checks replications of changes on nodes
#

# Basic logical replication test
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $result;
my $stdout;
my $stderr;

#
# In this test three-way logical replication group
# will be created with following cases:
#
# - Case1: There is no data in any of the nodes
#

#
# Initialize all nodes
#

my $node1 = PostgreSQL::Test::Cluster->new('node1');
$node1->init(allows_streaming => 'logical');
$node1->start;

my $node2 = PostgreSQL::Test::Cluster->new('node2');
$node2->init(allows_streaming => 'logical');
$node2->start;

my $node3 = PostgreSQL::Test::Cluster->new('node3');
$node3->init(allows_streaming => 'logical');
$node3->start;

my $group_name = 'test_group';

my $node1_connstr = $node1->connstr . ' dbname=postgres';
my $node2_connstr = $node2->connstr . ' dbname=postgres';
my $node3_connstr = $node3->connstr . ' dbname=postgres';

#
# Create tables that will share changes on all nodes
# They will be used for testing
#

$node1->safe_psql(
	'postgres',
	'CREATE TABLE foo (id int PRIMARY KEY)');
$node2->safe_psql(
	'postgres',
	'CREATE TABLE foo (id int PRIMARY KEY)');
$node3->safe_psql(
	'postgres',
	'CREATE TABLE foo (id int PRIMARY KEY)');

#
# - Case1: There is no data in any of the nodes
#

## Create a logical replication group on node1

$node1->safe_psql(
	'postgres',
	"SELECT lrg_create('$group_name', 'FOR ALL TABLES', '$node1_connstr', 'node1')");

## Attach node2, node3 to the logical replication group

$node2->safe_psql(
	'postgres',
	"SELECT lrg_node_attach('$group_name', '$node2_connstr', '$node1_connstr', 'node2')");

$node2->safe_psql('postgres', "SELECT lrg_wait()");

$node3->safe_psql(
	'postgres',
	"SELECT lrg_node_attach('$group_name', '$node3_connstr', '$node1_connstr', 'node3')");

$node3->safe_psql('postgres', "SELECT lrg_wait()");

## Now logical replication system has been created. All changes will be shared to all nodes.

my $subname12 = $node1->safe_psql('postgres',
	"SELECT subname FROM pg_subscription WHERE subconninfo = '$node2_connstr'");
my $subname13 = $node1->safe_psql('postgres',
	"SELECT subname FROM pg_subscription WHERE subconninfo = '$node3_connstr'");

my $subname21 = $node2->safe_psql('postgres',
	"SELECT subname FROM pg_subscription WHERE subconninfo = '$node1_connstr'");
my $subname23 = $node2->safe_psql('postgres',
	"SELECT subname FROM pg_subscription WHERE subconninfo = '$node3_connstr'");

my $subname31 = $node3->safe_psql('postgres',
	"SELECT subname FROM pg_subscription WHERE subconninfo = '$node1_connstr'");
my $subname32 = $node3->safe_psql('postgres',
	"SELECT subname FROM pg_subscription WHERE subconninfo = '$node2_connstr'");

## Inserted data on node1 will appear on node2, node3.

$node1->safe_psql('postgres', "INSERT INTO foo VALUES (1)");

verify_data($node1, $node2, $node3, '1');

## Inserted data on node2 will appear on node1, node3.

$node2->safe_psql('postgres', "INSERT INTO foo VALUES (2)");

verify_data($node1, $node2, $node3, '1
2');

## Inserted data on node3 will appear on node1, node2.

$node3->safe_psql('postgres', "INSERT INTO foo VALUES (3)");

verify_data($node1, $node2, $node3, '1
2
3');


## Updated data on node1 will appear on node2, node3.

$node1->safe_psql('postgres', "UPDATE foo SET id = 4 WHERE id = 1");

verify_data($node1, $node2, $node3, '2
3
4');

## Updated data on node2 will appear on node1, node3.

$node2->safe_psql('postgres', "UPDATE foo SET id = 5 WHERE id = 2");

verify_data($node1, $node2, $node3, '3
4
5');

## Updated data on node3 will appear on node1, node2.

$node2->safe_psql('postgres', "UPDATE foo SET id = 6 WHERE id = 3");

verify_data($node1, $node2, $node3, '4
5
6');

## Deleted data on node1 will be removed on node2, node3.

$node1->safe_psql('postgres', "DELETE FROM foo WHERE id = 6");

verify_data($node1, $node2, $node3, '4
5');

## Deleted data on node2 will be removed on node1, node3.

$node2->safe_psql('postgres', "DELETE FROM foo WHERE id = 5");

verify_data($node1, $node2, $node3, '4');

## Deleted data on node3 will be removed on node1, node2.

$node2->safe_psql('postgres', "DELETE FROM foo WHERE id = 4");

verify_data($node1, $node2, $node3, '');


# shutdown
$node1->stop('fast');
$node2->stop('fast');

done_testing();

# Subroutine to verify the data is replicated successfully.
sub verify_data
{
	my ($node1, $node2, $node3, $expect) = @_;

	$node1->wait_for_catchup($subname21);
	$node1->wait_for_catchup($subname31);
	$node2->wait_for_catchup($subname12);
	$node2->wait_for_catchup($subname32);
	$node3->wait_for_catchup($subname13);
	$node3->wait_for_catchup($subname23);

	# check that data is replicated to all the nodes
	$result =
	  $node1->safe_psql('postgres', "SELECT * FROM foo ORDER BY 1;");
	is($result, qq($expect),
	   'Data is replicated as expected'
	);

	$result =
	  $node2->safe_psql('postgres', "SELECT * FROM foo ORDER BY 1;");
	is($result, qq($expect),
	   'Data is replicated as expected'
	);

	$result =
	  $node3->safe_psql('postgres', "SELECT * FROM foo ORDER BY 1;");
	is($result, qq($expect),
	   'Data is replicated as expected'
	);
}
