#!/usr/bin/perl

# Copyright (c) 2025, PostgreSQL Global Development Group

# Test for the permit_unlogged_tables GUC parameter
#
# This test verifies that:
# 1. Unlogged tables can be created when permit_unlogged_tables=true (default)
# 2. Unlogged tables are rejected when permit_unlogged_tables=false
# 3. Configuration changes can be applied via reload without restart
# 4. ALTER TABLE SET UNLOGGED is also controlled by the parameter
# 5. ALTER TABLE SET LOGGED works regardless of the parameter

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize a test cluster with permit_unlogged_tables enabled by default (true)
my $node = PostgreSQL::Test::Cluster->new('permit_unlogged_test');
$node->init();
$node->start();

# Test 1: Verify default behavior - unlogged tables should be allowed by default
note("Testing default behavior - unlogged tables should be allowed");

# Verify the default value is true
my $result = $node->safe_psql('postgres',
    "SHOW permit_unlogged_tables;");
is($result, 'on', 'permit_unlogged_tables defaults to on');

# Create an unlogged table - should succeed
$node->safe_psql('postgres',
    "CREATE UNLOGGED TABLE test_unlogged1 (id int, data text);");

# Verify the table was created as unlogged
$result = $node->safe_psql('postgres',
    "SELECT relpersistence FROM pg_class WHERE relname = 'test_unlogged1';");
is($result, 'u', 'table created as unlogged');

# Insert some data
$node->safe_psql('postgres',
    "INSERT INTO test_unlogged1 VALUES (1, 'test data');");

# Create a regular table for later testing
$node->safe_psql('postgres',
    "CREATE TABLE test_regular1 (id int, data text);");

# Test ALTER TABLE SET UNLOGGED on regular table - should succeed
$node->safe_psql('postgres',
    "ALTER TABLE test_regular1 SET UNLOGGED;");

$result = $node->safe_psql('postgres',
    "SELECT relpersistence FROM pg_class WHERE relname = 'test_regular1';");
is($result, 'u', 'regular table converted to unlogged');

note("Initial tests with permit_unlogged_tables=on completed successfully");

# Test 2: Disable unlogged tables via configuration change
note("Testing configuration change to disable unlogged tables");

# Update postgresql.conf to disable unlogged tables
$node->append_conf('postgresql.conf', "permit_unlogged_tables = false");

# Reload configuration (SIGHUP) - should not require restart
$node->reload();

# Give the reload a moment to take effect
sleep(1);

# Verify the setting changed
$result = $node->safe_psql('postgres',
    "SHOW permit_unlogged_tables;");
is($result, 'off', 'permit_unlogged_tables changed to off after reload');

# Test 3: Verify unlogged table creation fails when disabled
note("Testing unlogged table creation fails when permit_unlogged_tables=off");

# Try to create an unlogged table - should fail
my ($ret, $stdout, $stderr) = $node->psql('postgres',
    "CREATE UNLOGGED TABLE test_unlogged2 (id int, data text);");

isnt($ret, 0, 'CREATE UNLOGGED TABLE fails when permit_unlogged_tables=off');
like($stderr, qr/Unlogged tables are not permitted/,
    'correct error message for CREATE UNLOGGED TABLE');

# Test 4: Verify ALTER TABLE SET UNLOGGED fails when disabled
note("Testing ALTER TABLE SET UNLOGGED fails when permit_unlogged_tables=off");

# Create a regular table first
$node->safe_psql('postgres',
    "CREATE TABLE test_regular2 (id int, data text);");

# Try to convert it to unlogged - should fail
($ret, $stdout, $stderr) = $node->psql('postgres',
    "ALTER TABLE test_regular2 SET UNLOGGED;");

isnt($ret, 0, 'ALTER TABLE SET UNLOGGED fails when permit_unlogged_tables=off');
like($stderr, qr/Unlogged tables are not permitted/,
    'correct error message for ALTER TABLE SET UNLOGGED');

# Verify the table is still regular
$result = $node->safe_psql('postgres',
    "SELECT relpersistence FROM pg_class WHERE relname = 'test_regular2';");
is($result, 'p', 'table remains regular after failed ALTER SET UNLOGGED');

# Test 5: Verify ALTER TABLE SET LOGGED still works
note("Testing ALTER TABLE SET LOGGED works regardless of permit_unlogged_tables setting");

# Convert the existing unlogged table to logged - should always work
$node->safe_psql('postgres',
    "ALTER TABLE test_regular1 SET LOGGED;");

$result = $node->safe_psql('postgres',
    "SELECT relpersistence FROM pg_class WHERE relname = 'test_regular1';");
is($result, 'p', 'unlogged table converted to logged');

# Test 6: Verify existing unlogged tables continue to work
note("Testing existing unlogged tables continue to work when permit_unlogged_tables=off");

# The existing unlogged table should still be accessible
$result = $node->safe_psql('postgres',
    "SELECT COUNT(*) FROM test_unlogged1;");
is($result, '1', 'existing unlogged table is still accessible');

# Should be able to insert/update/delete
$node->safe_psql('postgres',
    "INSERT INTO test_unlogged1 VALUES (2, 'more test data');");

$result = $node->safe_psql('postgres',
    "SELECT COUNT(*) FROM test_unlogged1;");
is($result, '2', 'can insert into existing unlogged table');

# Test 7: Re-enable unlogged tables and verify they work again
note("Testing re-enabling unlogged tables");

# Update postgresql.conf to re-enable unlogged tables
$node->append_conf('postgresql.conf', "permit_unlogged_tables = true");

# Reload configuration
$node->reload();

# Give the reload a moment to take effect
sleep(1);

# Verify the setting changed back
$result = $node->safe_psql('postgres',
    "SHOW permit_unlogged_tables;");
is($result, 'on', 'permit_unlogged_tables re-enabled');

# Now unlogged table creation should work again
$node->safe_psql('postgres',
    "CREATE UNLOGGED TABLE test_unlogged3 (id int, data text);");

$result = $node->safe_psql('postgres',
    "SELECT relpersistence FROM pg_class WHERE relname = 'test_unlogged3';");
is($result, 'u', 'unlogged table creation works again after re-enabling');

# Test ALTER TABLE SET UNLOGGED should work again too
$node->safe_psql('postgres',
    "ALTER TABLE test_regular2 SET UNLOGGED;");

$result = $node->safe_psql('postgres',
    "SELECT relpersistence FROM pg_class WHERE relname = 'test_regular2';");
is($result, 'u', 'ALTER TABLE SET UNLOGGED works again after re-enabling');

# Test 8: Test parameter setting via SQL (should be rejected)
note("Testing permit_unlogged_tables cannot be set via SQL session");

# Try to set via SET command - should fail since it's PGC_SIGHUP
($ret, $stdout, $stderr) = $node->psql('postgres',
    "SET permit_unlogged_tables = false;");

isnt($ret, 0, 'SET permit_unlogged_tables fails (PGC_SIGHUP)');
like($stderr, qr/parameter .* cannot be changed/,
    'correct error message for SET attempt');

# Test 9: Test with different table types
note("Testing with different table types");

# Test with partitioned tables
$node->safe_psql('postgres',
    "CREATE TABLE test_partitioned (id int, data text) PARTITION BY RANGE (id);");

# Try to create unlogged partition when enabled - should work
$node->safe_psql('postgres',
    "CREATE UNLOGGED TABLE test_partition1 PARTITION OF test_partitioned FOR VALUES FROM (1) TO (100);");

$result = $node->safe_psql('postgres',
    "SELECT relpersistence FROM pg_class WHERE relname = 'test_partition1';");
is($result, 'u', 'unlogged partition created successfully');

# Disable unlogged tables again
$node->append_conf('postgresql.conf', "permit_unlogged_tables = false");
$node->reload();
sleep(1);

# Try to create another unlogged partition - should fail
($ret, $stdout, $stderr) = $node->psql('postgres',
    "CREATE UNLOGGED TABLE test_partition2 PARTITION OF test_partitioned FOR VALUES FROM (100) TO (200);");

isnt($ret, 0, 'CREATE UNLOGGED partition fails when permit_unlogged_tables=off');

# Cleanup
note("Cleaning up test objects");

$node->safe_psql('postgres', "DROP TABLE IF EXISTS test_unlogged1, test_unlogged3, test_regular1, test_regular2, test_partition1, test_partitioned CASCADE;");

# Stop the node
$node->stop();

done_testing();
