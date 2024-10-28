
# Copyright (c) 2024, PostgreSQL Global Development Group

# Test ReorderBuffer compression
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub test_reorderbuffer_compression
{
	my ($node_publisher, $node_subscriber, $appname, $compression, $compression_rate) = @_;

	# Set subscriber's spill_compression option
	$node_subscriber->safe_psql('postgres',
		"ALTER SUBSCRIPTION tap_sub SET (spill_compression = $compression)");

	# Make sure the table is empty
	$node_publisher->safe_psql('postgres', 'TRUNCATE test_tab');

	# Reset replication slot stats
	$node_publisher->safe_psql('postgres',
		"SELECT pg_stat_reset_replication_slot('tap_sub')");

	# Insert 1 million rows in the table
	$node_publisher->safe_psql('postgres',
		"INSERT INTO test_tab SELECT i, 'Message number #'||i::TEXT FROM generate_series(1, 1000000) as i"
	);

	$node_publisher->wait_for_catchup($appname);

	# Check if table content is replicated
	my $result =
	  $node_subscriber->safe_psql('postgres',
		"SELECT count(*) FROM test_tab");
	is($result, qq(1000000), 'check data was copied to subscriber');

	# Check if the transaction was spilled on disk
	my $res_stats =
	  $node_publisher->safe_psql('postgres',
		"SELECT spill_txns FROM pg_catalog.pg_stat_get_replication_slot('tap_sub');");
	is($res_stats, qq(1), 'check if the transaction was spilled on disk');

	# Check the compression ratio
	my $res_comp_rate =
	  $node_publisher->safe_psql('postgres',
		"SELECT ((1 - spill_write_bytes::FLOAT / spill_bytes::FLOAT) * 100)::INT FROM pg_catalog.pg_stat_get_replication_slot('tap_sub');");
	ok($res_comp_rate >= $compression_rate, "check if the compression rate (spill_compression = '$compression') is greater than or equal to $compression_rate");
}

# Create publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf('postgresql.conf',
	'logical_decoding_work_mem = 64');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

# Setup structure on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_tab (a int primary key, b text)");

# Setup structure on subscriber
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_tab (a int primary key, b text)");

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR TABLE test_tab");

my $appname = 'tap_sub';

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub WITH (streaming = off)"
);

# No data compression expected, compression ratio is 0
test_reorderbuffer_compression($node_publisher, $node_subscriber, $appname,
	'off', 0);
# Compression ratio greater than or equal to 30% for pglz
test_reorderbuffer_compression($node_publisher, $node_subscriber, $appname,
	'pglz', 30);
SKIP:
{
	skip "LZ4 not supported by this build", 2 if ($ENV{with_lz4} ne 'yes');
	# Compression ratio greater than or equal to 70% for lz4
	test_reorderbuffer_compression($node_publisher, $node_subscriber, $appname,
		'lz4', 70);
}
SKIP:
{
	skip "ZSTD not supported by this build", 2 if ($ENV{with_zstd} ne 'yes');
	# Compression ratio greater than or equal to 80% for zstd
	test_reorderbuffer_compression($node_publisher, $node_subscriber, $appname,
		'zstd', 80);
}

$node_subscriber->stop;
$node_publisher->stop;

done_testing();
