
# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Test in-transaction stats flushing mechanism.
#
# This test verifies that FLUSH_IN_TRANSACTION stats are periodically flushed
# to shared memory while a transaction is idle between commands.  Uses an
# injection point to reduce the flush interval from 10 seconds to 1 second
# for faster testing.
#
# We test one representative of each stats kind: relation stats (seq_scan)
# for variable-numbered stats, and WAL for fixed-sized stats.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init();
$node->append_conf('postgresql.conf', 'stats_fetch_consistency = none');
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

# Attach injection point to reduce the in-transaction stats flush interval
# to 1 second.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('in-transaction-stats-short-interval', 'error');");

# Create test table
$node->safe_psql('postgres',
	'CREATE TABLE test_in_txn_stats(a int) WITH (autovacuum_enabled = off);');
$node->safe_psql('postgres',
	'INSERT INTO test_in_txn_stats SELECT generate_series(1, 1000);');

# Force flush and get baseline stats
$node->safe_psql('postgres', 'SELECT pg_stat_force_next_flush();');
my $seq_scan_before = $node->safe_psql('postgres',
	"SELECT seq_scan FROM pg_stat_user_tables WHERE relname = 'test_in_txn_stats';");

# Test that seq_scan stats (variable-numbered) are flushed mid-transaction
# via the in-transaction periodic flush mechanism.
my $result = $node->safe_psql('postgres', q{
BEGIN;
SELECT COUNT(*) FROM test_in_txn_stats;
SELECT COUNT(*) FROM test_in_txn_stats;
SELECT pg_sleep(2);
SELECT seq_scan FROM pg_stat_user_tables WHERE relname = 'test_in_txn_stats';
});

my @lines = split(/\n/, $result);
my $seq_scan_mid_txn = $lines[-1];

ok($seq_scan_mid_txn > $seq_scan_before,
	"seq_scan stats flushed during transaction (before: $seq_scan_before, mid-txn: $seq_scan_mid_txn)");

# Test WAL stats (fixed-sized) flushing during transaction.
$node->safe_psql('postgres', 'SELECT pg_stat_reset_shared(\'wal\');');
my $wal_records_before = $node->safe_psql('postgres',
	"SELECT wal_records FROM pg_stat_wal;");

$result = $node->safe_psql('postgres', q{
BEGIN;
INSERT INTO test_in_txn_stats SELECT generate_series(1, 1000);
SELECT pg_sleep(2);
SELECT wal_records FROM pg_stat_wal;
});

@lines = split(/\n/, $result);
my $wal_records_mid_txn = $lines[-1];

ok($wal_records_mid_txn > $wal_records_before,
	"WAL stats flushed during transaction (before: $wal_records_before, mid-txn: $wal_records_mid_txn)");

# Cleanup
$node->safe_psql('postgres', 'DROP TABLE test_in_txn_stats;');

done_testing();
