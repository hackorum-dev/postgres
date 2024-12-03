
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Tests to show spill file accumulation when a prepared transaction
# that is not committed, holds back the lowest xid.
# And there are incomplete large transactions that get aborted
# on an unclean shutdown, causing spill files to take up disk space.
#
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf('postgresql.conf',
	"max_prepared_transactions = 10
	 logical_decoding_work_mem = 64kB");
$node_publisher->start;

# Create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->append_conf('postgresql.conf',
	"max_prepared_transactions = 10");
$node_subscriber->start;

# Create some preexisting content on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rep (a int primary key, value text)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rep SELECT x, md5(x::text) FROM generate_series(1,10) x");

# Setup structure on subscriber
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rep (a int primary key, value text)");

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR ALL TABLES");

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub WITH (streaming = off)"
);

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

my $result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_rep");
is($result, qq(10), 'initial data synced for first sub');

# Create a prepared transaction that holds back the min xid.
$node_publisher->safe_psql('postgres',
	qq[
		BEGIN;
		INSERT INTO tab_rep SELECT x, md5(x::text) FROM generate_series(11, 100) x;
		PREPARE TRANSACTION 'TEST1';
	]
);

my $psql_session = $node_publisher->background_psql('postgres');
$psql_session->query_safe(
	qq[
		BEGIN;
		INSERT INTO tab_rep SELECT x, md5(x::text) FROM generate_series(101, 1000000) x;
	]
);
my $running_xid=$psql_session->query_safe("SELECT txid_current();");

sleep(10);

$node_publisher->stop('immediate');
$node_publisher->start;

sleep(5);
$result=$node_publisher->safe_psql('postgres',
	qq[
		SELECT count(*) = 0 FROM
		(SELECT pg_ls_dir('pg_replslot/tap_sub')) AS s
		WHERE s.pg_ls_dir LIKE '%xid-] . $running_xid . q[%']
	);
is($result, 't', 'no spill files left over from incomplete transaction');

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rep values(generate_series(101, 200));");

$result=$node_publisher->safe_psql('postgres', q[select count(*) = 0 from
	(select pg_ls_dir('pg_replslot/tap_sub')) as s
	where s.pg_ls_dir like '%xid-] . $running_xid . q[%']);
is($result, 't', 'no spill files left over from incomplete transaction');

$node_publisher->safe_psql('postgres', "ROLLBACK PREPARED 'TEST1'");

$node_publisher->poll_query_until('postgres', q[select count(*) = 0 from
    (select pg_ls_dir('pg_replslot/tap_sub')) as s
    where s.pg_ls_dir like '%xid-] . $running_xid . q[%'])
  or die "Timed out while waiting for spill files to disappear";

$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub");

# check subscriptions are removed
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_subscription");
is($result, qq(0), 'second and third sub are dropped');

# remove the conflicting data
$node_subscriber->safe_psql('postgres', "DELETE FROM tab_rep;");
$node_publisher->safe_psql('postgres', "DROP TABLE tab_rep");
$node_subscriber->safe_psql('postgres', "DROP TABLE tab_rep");

$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
