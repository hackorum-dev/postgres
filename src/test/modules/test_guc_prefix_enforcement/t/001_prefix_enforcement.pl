# Copyright (c) 2026, PostgreSQL Global Development Group

# Test GUC prefix reservation enforcement modes

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

#
# Test 1: Default mode (off) - all extensions should load without errors
#
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'test_guc_prefix_enforcement'");
$node->start;

my $result = $node->safe_psql('postgres', 'SHOW test_guc_prefix_enforcement.test_var');
is($result, 'default', 'good extension loads with default enforcement (off)');

$node->stop;

#
# Test 2: warn mode with good extension - should load with no warnings
#
$node = PostgreSQL::Test::Cluster->new('warn_good');
$node->init;
$node->append_conf('postgresql.conf', "guc_prefix_enforcement = 'warn'");
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'test_guc_prefix_enforcement'");
$node->start;

$result = $node->safe_psql('postgres', 'SHOW test_guc_prefix_enforcement.test_var');
is($result, 'default', 'good extension loads with warn enforcement');

# Verify no warnings were emitted
my $log = $node->logfile;
my $log_contents = slurp_file($log);
unlike($log_contents, qr/WARNING.*MarkGUCPrefixReserved/,
	 'no warning about MarkGUCPrefixReserved for good extension');
unlike($log_contents, qr/WARNING.*reserved prefix/,
	 'no warning about reserved prefix for good extension');

$node->stop;

#
# Test 3: warn mode with no_reserve extension - should load with warning
#
$node = PostgreSQL::Test::Cluster->new('warn_no_reserve');
$node->init;
$node->append_conf('postgresql.conf', "guc_prefix_enforcement = 'warn'");
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'test_guc_no_reserve'");

# Start should succeed but log warnings
$node->start;
$result = $node->safe_psql('postgres', 'SHOW test_guc_no_reserve.some_var');
is($result, 'default', 'no_reserve extension loads with warn enforcement');

# Check log for warning
$log = $node->logfile;
$log_contents = slurp_file($log);
like($log_contents, qr/WARNING.*without calling MarkGUCPrefixReserved/,
	 'warn mode emits warning for extension without prefix reservation');

$node->stop;

#
# Test 4: strict mode with no_reserve extension - should fail to start
#
$node = PostgreSQL::Test::Cluster->new('strict_no_reserve');
$node->init;
$node->append_conf('postgresql.conf', "guc_prefix_enforcement = 'strict'");
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'test_guc_no_reserve'");

# Start should fail
my $ret = $node->start(fail_ok => 1);
is($ret, 0, 'strict mode prevents startup with non-compliant extension');

$log = $node->logfile;
$log_contents = slurp_file($log);
like($log_contents, qr/FATAL.*without calling MarkGUCPrefixReserved/,
	 'strict mode emits error for extension without prefix reservation');

#
# Test 5: prefix mode with wrong_prefix extension - should fail to start
#
$node = PostgreSQL::Test::Cluster->new('prefix_wrong');
$node->init;
$node->append_conf('postgresql.conf', "guc_prefix_enforcement = 'prefix'");
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'test_guc_wrong_prefix'");

# Start should fail
$ret = $node->start(fail_ok => 1);
is($ret, 0, 'prefix mode prevents startup with wrong prefix extension');

$log = $node->logfile;
$log_contents = slurp_file($log);
like($log_contents, qr/FATAL.*outside any reserved prefix/,
	 'prefix mode emits error for extension defining GUC outside reserved prefix');

#
# Test 6: warn mode with no_prefix extension - should load with warning
# (extension reserves a prefix but defines variable without any prefix)
#
$node = PostgreSQL::Test::Cluster->new('warn_no_prefix');
$node->init;
$node->append_conf('postgresql.conf', "guc_prefix_enforcement = 'warn'");
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'test_guc_no_prefix'");
$node->start;

$result = $node->safe_psql('postgres', 'SHOW test_guc_no_prefix_var');
is($result, 'default', 'no_prefix extension loads with warn enforcement');

$log = $node->logfile;
$log_contents = slurp_file($log);
like($log_contents, qr/WARNING.*outside any reserved prefix/,
	 'warn mode emits warning for extension defining GUC without prefix');

$node->stop;

#
# Test 7: prefix mode with no_prefix extension - should fail to start
# (extension reserves a prefix but defines variable without any prefix)
#
$node = PostgreSQL::Test::Cluster->new('prefix_no_prefix');
$node->init;
$node->append_conf('postgresql.conf', "guc_prefix_enforcement = 'prefix'");
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'test_guc_no_prefix'");

# Start should fail
$ret = $node->start(fail_ok => 1);
is($ret, 0, 'prefix mode prevents startup with unprefixed variable');

$log = $node->logfile;
$log_contents = slurp_file($log);
like($log_contents, qr/FATAL.*outside any reserved prefix/,
	 'prefix mode emits error for extension defining GUC without prefix');

#
# Test 8: verify good extension works in strict mode
#
$node = PostgreSQL::Test::Cluster->new('strict_good');
$node->init;
$node->append_conf('postgresql.conf', "guc_prefix_enforcement = 'strict'");
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'test_guc_prefix_enforcement'");
$node->start;

$result = $node->safe_psql('postgres', 'SHOW test_guc_prefix_enforcement.test_var');
is($result, 'default', 'good extension loads successfully in strict mode');

# Verify no warnings or errors were emitted
$log = $node->logfile;
$log_contents = slurp_file($log);
unlike($log_contents, qr/(WARNING|FATAL).*MarkGUCPrefixReserved/,
	 'no warning/error about MarkGUCPrefixReserved for good extension in strict mode');
unlike($log_contents, qr/(WARNING|FATAL).*reserved prefix/,
	 'no warning/error about reserved prefix for good extension in strict mode');

$node->stop;

done_testing();
