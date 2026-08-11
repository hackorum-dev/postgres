# Copyright (c) 2025-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

###
# Test allocating memory after startup, i.e. when the library is not
# in shared_preload_libraries
###
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

# Test failure when the request is larger than the memory reserved for
# after-startup requests.
$node->start;
my (undef, undef, $oom_stderr) = $node->psql("postgres", q[
SET test_shmem.area_size = '128kB';
CREATE EXTENSION test_shmem;]);
like($oom_stderr, qr/not enough shared memory/,
	"an after-startup request larger than the reserve fails");

# A failure in the requesting shared memory should not affect server
# availability. We should still be able to try to create the extension again.
# Test a failure in initialization of the shared memory area.
SKIP:
{
	skip "injection points not supported by this build", 2
	  if $ENV{enable_injection_points} ne 'yes';
	$node->safe_psql("postgres", "CREATE EXTENSION injection_points;");
	$node->safe_psql("postgres",
		"SELECT injection_points_attach('test-shmem-init', 'error');");
	ok($node->psql("postgres", "CREATE EXTENSION test_shmem;"),
		"request callback failure is reported on both attempts");
	$node->safe_psql("postgres",
		"SELECT injection_points_detach('test-shmem-init');");
}

# The server should still be available and verify that the request succeeds for
# smaller request.
$node->safe_psql("postgres", q[
SET test_shmem.area_size = default;
CREATE EXTENSION test_shmem;]);

# Check that the attach counter is incremented on a new connection
my $attach_count1 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");
my $attach_count2 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");
cmp_ok($attach_count2, '>', $attach_count1,
	"attach callback is called in each backend");
$node->stop;

###
# Test that loading via shared_preload_libraries also works, even for large request.
###
$node->append_conf('postgresql.conf', "test_shmem.area_size = '128kB'");
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'test_shmem'");
$node->start;

# When loaded via shared_preload_libraries, the attach callback is
# called or not, depending on whether this is an EXEC_BACKEND build.
my $exec_backend =
  $node->safe_psql("postgres", "SHOW debug_exec_backend;") eq 'on';
$attach_count1 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");
$attach_count2 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");

if ($exec_backend)
{
	cmp_ok($attach_count2, '>', $attach_count1,
		"attach callback is called in each backend when loaded via shared_preload_libraries"
	);
}
else
{
	ok( $attach_count1 == 0 && $attach_count2 == 0,
		"attach callback is not called when loaded via shared_preload_libraries"
	);
}

$node->stop;
done_testing();
