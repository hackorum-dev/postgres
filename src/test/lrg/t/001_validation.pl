
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
# In this test three nodes are used, called node1, node2, and node3.
# After checking validations of APIs, N-way logical replication
# group is created with following cases:
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

#
# Input check for APIs
#

my $group_name = 'test_group';

my $node1_connstr = $node1->connstr . ' dbname=postgres';
my $node2_connstr = $node2->connstr . ' dbname=postgres';
my $node3_connstr = $node3->connstr . ' dbname=postgres';

##
## Tests for lrg_create
##

$result = $node1->safe_psql('postgres', "SELECT COUNT(*) FROM pg_lrg_info");
is($result, qq(0), 'check the initialized node does not belong to any group');

($result, $stdout, $stderr) = $node1->psql(
	'postgres', "SELECT lrg_create('$group_name', 'WRONG INPUT', '$node1_connstr', 'node1')");
like
(
	$stderr,
	qr/ERROR:  cannot create a node group
DETAIL:  Only 'FOR ALL TABLES' is supported as publication type./,
	"check only one publication type is allowed"
);

$node1->safe_psql(
	'postgres',
	"SELECT lrg_create('$group_name', 'FOR ALL TABLES', '$node1_connstr', 'node1')");

$node1->safe_psql('postgres', "SELECT lrg_wait()");

($result, $stdout, $stderr) = $node1->psql(
	'postgres', "SELECT lrg_create('$group_name', 'FOR ALL TABLES', '$node1_connstr', 'node1')");
like
(
	$stderr,
	qr/ERROR:  could not create a node group
DETAIL:  This node was already a member of $group_name.
HINT:  You need to detach from or drop the group./,
	"check duplicated calling of lrg_create is not allowed"
);

##
## Tests for lrg_node_attach
##

($result, $stdout, $stderr) = $node2->psql(
	'postgres', "SELECT lrg_node_attach('WRONG GROUP', '$node2_connstr', '$node1_connstr', 'node2')");
like
(
	$stderr,
	qr/ERROR:  could not attach to the node group
DETAIL:  Upstream node is not a member of specified group./,
	"sanity check for group_name while attaching"
);

($result, $stdout, $stderr) = $node2->psql(
	'postgres', "SELECT lrg_node_attach('$group_name', '$node2_connstr', 'wrong connection string', 'node2')");
like
(
	$stderr,
	qr/ERROR:  could not connect to the server
HINT:  Please check the connection string and health of destination./,
	"sanity check for upstream connection string"
);

$node2->safe_psql(
	'postgres',
	"SELECT lrg_node_attach('$group_name', '$node2_connstr', '$node1_connstr', 'node2')");

$node2->safe_psql('postgres', "SELECT lrg_wait()");

($result, $stdout, $stderr) = $node2->psql(
	'postgres', "SELECT lrg_node_attach('$group_name', '$node2_connstr', 'wrong connection string', 'node2')");
like
(
	$stderr,
	qr/ERROR:  could not attach to a node group
DETAIL:  This node was already a member of $group_name.
HINT:  You need to detach from or drop the group./,
	"check duplicated calling of lrg_node_attach is not allowed"
);

($result, $stdout, $stderr) = $node2->psql(
	'postgres', "SELECT lrg_create('$group_name', 'FOR ALL TABLES', '$node1_connstr', 'node1')");
like
(
	$stderr,
	qr/ERROR:  could not create a node group
DETAIL:  This node was already a member of $group_name.
HINT:  You need to detach from or drop the group./,
	"check lrg_create cannot be called if this node is already a member of a node group"
);

###
### do lrg_node_attach() in node3. It will be used for testing "force" detach.
###

$node3->safe_psql(
	'postgres',
	"SELECT lrg_node_attach('$group_name', '$node3_connstr', '$node1_connstr', 'node3')");

$node3->safe_psql('postgres', "SELECT lrg_wait()");


##
## Tests for lrg_node_detach
##

($result, $stdout, $stderr) = $node2->psql(
	'postgres', "SELECT lrg_node_detach('WRONG GROUP', 'node2')");
like
(
	$stderr,
	qr/ERROR:  could not detach from the node group
DETAIL:  This node was in $group_name, but WRONG GROUP is specified./,
	"sanity check for group_name while detaching"
);

$node2->safe_psql(
	'postgres',
	"SELECT lrg_node_detach('$group_name', 'node2')");

$node2->safe_psql('postgres', "SELECT lrg_wait()");

($result, $stdout, $stderr) = $node2->psql(
	'postgres', "SELECT lrg_node_detach('$group_name', 'node2')");
like
(
	$stderr,
	qr/ERROR:  could not detach from the node group
DETAIL:  This node was in any node groups./,
	"duplicated calling is not allowed"
);

###
### Tests for "force" detach. It can be done even if specified node goes down
###

$node3->stop('fast');

$node1->safe_psql(
	'postgres',
	"SELECT lrg_node_detach('$group_name', 'node3', true)");

$node1->safe_psql('postgres', "SELECT lrg_wait()");

##
## Tests for lrg_drop
##

($result, $stdout, $stderr) = $node1->psql(
	'postgres', "SELECT lrg_drop('WRONG GROUP')");
like
(
	$stderr,
	qr/ERROR:  could not drop the node group
DETAIL:  This node was in $group_name, but WRONG GROUP is specified./,
	"sanity check for group_name while dropping"
);

$node1->safe_psql(
	'postgres',
	"SELECT lrg_drop('$group_name')");

$node1->safe_psql('postgres', "SELECT lrg_wait()");

($result, $stdout, $stderr) = $node1->psql(
	'postgres', "SELECT lrg_drop('$group_name')");
like
(
	$stderr,
	qr/ERROR:  could not drop the node group
DETAIL:  This node was in any node groups./,
	"duplicated calling is not allowed"
);

# shutdown
$node1->stop('fast');
$node2->stop('fast');

done_testing();
