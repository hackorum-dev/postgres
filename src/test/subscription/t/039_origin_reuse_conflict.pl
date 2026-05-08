# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that roident reuse is detected as an update_origin_differs conflict.
#
# sub1 and sub2 both use copy_data=false (no tablesync, one origin each).
# Dropping sub1 and creating sub2 reuses the same roident.  A row written by
# sub1's apply worker has the same origin number as sub2, so only
# IsRoidentReused() can distinguish them.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_pub = PostgreSQL::Test::Cluster->new('publisher');
$node_pub->init(allows_streaming => 'logical');
$node_pub->start;

my $node_sub = PostgreSQL::Test::Cluster->new('subscriber');
$node_sub->init;
$node_sub->append_conf('postgresql.conf', "track_commit_timestamp = on");
$node_sub->start;

$node_pub->safe_psql('postgres', "CREATE TABLE t (a int PRIMARY KEY, b text)");
$node_sub->safe_psql('postgres', "CREATE TABLE t (a int PRIMARY KEY, b text)");

my $pubconn = $node_pub->connstr . ' dbname=postgres';
$node_pub->safe_psql('postgres', "CREATE PUBLICATION pub FOR TABLE t");

$node_sub->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1
	 CONNECTION '$pubconn'
	 PUBLICATION pub
	 WITH (copy_data = false)");

# INSERT flows through sub1's apply worker, stamping the row with roident 1.
$node_pub->safe_psql('postgres', "INSERT INTO t VALUES (1, 'a')");
$node_pub->wait_for_catchup('sub1');

# Drop sub1 (frees roident 1), sleep so sub2's ro_created > row's commit_ts.
$node_sub->safe_psql('postgres', "DROP SUBSCRIPTION sub1");
$node_sub->safe_psql('postgres', "SELECT pg_sleep(0.05)");

$node_sub->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub2
	 CONNECTION '$pubconn'
	 PUBLICATION pub
	 WITH (copy_data = false)");

my $log_start = -s $node_sub->logfile;

# The row has origin=roident1 from sub1 with commit_ts=T1.  sub2 also has
# roident1 but ro_created=T2 > T1, so IsRoidentReused() fires.
$node_pub->safe_psql('postgres', "UPDATE t SET b = 'b' WHERE a = 1");
$node_pub->wait_for_catchup('sub2');

my $logfile = slurp_file($node_sub->logfile, $log_start);
like(
	$logfile,
	qr/conflict detected on relation "public\.t": conflict=update_origin_differs/,
	'update_origin_differs conflict reported for reused roident');

my $val = $node_sub->safe_psql('postgres', "SELECT b FROM t WHERE a = 1");
is($val, 'b', 'row updated to latest value after conflict');

$node_sub->safe_psql('postgres', "DROP SUBSCRIPTION sub2");

done_testing();
