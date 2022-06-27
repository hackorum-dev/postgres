# Copyright (c) 2022, PostgreSQL Global Development Group

#
# Tests for constructing a logical replication groups
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
# In this test two-way logical replication group
# will be created. A node will attach, detach, and attach
# again to the same node group. Also, catalogs related with
# LRG will be checked on both nodes
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

my $group_name = 'test_group';

my $node1_connstr = $node1->connstr . ' dbname=postgres';
my $node2_connstr = $node2->connstr . ' dbname=postgres';

# Create a logical replication group on node1

$node1->safe_psql(
	'postgres',
	"SELECT lrg_create('$group_name', 'FOR ALL TABLES', '$node1_connstr', 'node1')");

## Check its catalog related with LRG

$result = $node1->safe_psql('postgres', "SELECT groupname, pub_type FROM pg_lrg_info");
is($result, qq($group_name|FOR ALL TABLES), 'check creating group has been succeeded');

$result = $node1->safe_psql('postgres', "SELECT nodename FROM pg_lrg_nodes");
is($result, qq(node1), 'check this node has attached to the group');

$node1->safe_psql('postgres', "SELECT lrg_wait()");

$result = $node1->safe_psql('postgres', "SELECT COUNT(*) FROM pg_lrg_pub");
is($result, qq(1), 'check a publication has been associated with the group');

# Attach node2 to the logical replication group

$node2->safe_psql(
	'postgres',
	"SELECT lrg_node_attach('$group_name', '$node2_connstr', '$node1_connstr', 'node2')");

$node2->safe_psql('postgres', "SELECT lrg_wait()");

## Check its catalog related with LRG

$result = $node2->safe_psql('postgres', "SELECT groupname, pub_type FROM pg_lrg_info");
is($result, qq($group_name|FOR ALL TABLES), 'check creating group has been succeeded');

$result = $node2->safe_psql('postgres', "SELECT COUNT(*) FROM pg_lrg_nodes");
is($result, qq(2), 'check this node has attached to the group');

$result = $node2->safe_psql('postgres', "SELECT COUNT(*) FROM pg_lrg_pub");
is($result, qq(1), 'check a publication has been associated with the group');

$result = $node1->safe_psql('postgres', "SELECT COUNT(*) FROM pg_lrg_sub");
is($result, qq(1), 'check a subscription has been associated with the group');

$result = $node2->safe_psql('postgres', "SELECT COUNT(*) FROM pg_lrg_sub");
is($result, qq(1), 'check a subscription has been associated with the group');

# Detach node2 once

$node2->safe_psql('postgres', "SELECT lrg_node_detach('$group_name', 'node2')");
$node2->safe_psql('postgres', "SELECT lrg_wait()");

## Check its catalog related with LRG

$result = $node2->safe_psql('postgres', "SELECT groupname, pub_type FROM pg_lrg_info");
is($result, qq(), 'check catalogs have been cleaned up');

$result = $node2->safe_psql('postgres', "SELECT COUNT(*) FROM pg_lrg_nodes");
is($result, qq(0), 'check catalogs have been cleaned up');

$result = $node2->safe_psql('postgres', "SELECT COUNT(*) FROM pg_lrg_pub");
is($result, qq(0), 'check catalogs have been cleaned up');

$result = $node1->safe_psql('postgres', "SELECT COUNT(*) FROM pg_lrg_sub");
is($result, qq(1), 'check a subscription has been deleted');

$result = $node2->safe_psql('postgres', "SELECT COUNT(*) FROM pg_lrg_sub");
is($result, qq(0), 'check catalogs have been cleaned up');

# ...and attach again

$node2->safe_psql(
	'postgres',
	"SELECT lrg_node_attach('$group_name', '$node2_connstr', '$node1_connstr', 'node2')");

$node2->safe_psql('postgres', "SELECT lrg_wait()");

## Check its catalog related with LRG

$result = $node2->safe_psql('postgres', "SELECT groupname, pub_type FROM pg_lrg_info");
is($result, qq($group_name|FOR ALL TABLES), 'check creating group has been succeeded');

$result = $node2->safe_psql('postgres', "SELECT COUNT(*) FROM pg_lrg_nodes");
is($result, qq(2), 'check this node has attached to the group');

$result = $node2->safe_psql('postgres', "SELECT COUNT(*) FROM pg_lrg_pub");
is($result, qq(1), 'check a publication has been associated with the group');

$result = $node1->safe_psql('postgres', "SELECT COUNT(*) FROM pg_lrg_sub");
is($result, qq(2), 'check a subscription has been associated with the group');

$result = $node2->safe_psql('postgres', "SELECT COUNT(*) FROM pg_lrg_sub");
is($result, qq(1), 'check a subscription has been associated with the group');

# shutdown
$node1->stop('fast');
$node2->stop('fast');

done_testing();
