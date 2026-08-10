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
$node->start;


SKIP:
{
	skip "injection points not supported by this build", 2
	  if $ENV{enable_injection_points} ne 'yes';
	$node->safe_psql("postgres", "CREATE EXTENSION injection_points;");
	$node->safe_psql("postgres",
		"SELECT injection_points_attach('shmem-after-startup-request', 'error');");
	my $session = $node->background_psql('postgres', on_error_stop => 0);
	my (undef, $failed1) = $session->query("CREATE EXTENSION test_shmem;");
	my (undef, $failed2) = $session->query("CREATE EXTENSION test_shmem;");
	$session->quit;
	ok($failed1 && $failed2,
		"request callback failure is reported on both attempts");
	is($node->safe_psql("postgres", "SELECT 1"), '1',
		"request callback failure does not crash the backend");
	$node->safe_psql("postgres",
		"SELECT injection_points_detach('shmem-after-startup-request');");
}

$node->stop;

my $oom_node = PostgreSQL::Test::Cluster->new('oom');
$oom_node->init;
$oom_node->start;
my (undef, undef, $oom_stderr) = $oom_node->psql("postgres", q[
SET test_shmem.area_size = '128kB';
CREATE EXTENSION test_shmem;]);
like($oom_stderr, qr/not enough shared memory/,
	"an after-startup request larger than the reserve fails");
$oom_node->stop;

my $preload_node = PostgreSQL::Test::Cluster->new('preload_large');
$preload_node->init;
$preload_node->append_conf('postgresql.conf', q[
test_shmem.area_size = '128kB'
shared_preload_libraries = 'test_shmem']);
$preload_node->start;
$preload_node->safe_psql("postgres", "CREATE EXTENSION test_shmem;");
is($preload_node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();"),
	'0', "the same request succeeds when test_shmem is preloaded");
$preload_node->stop;

$node = PostgreSQL::Test::Cluster->new('normal');
$node->init;
$node->start;
$node->safe_psql("postgres", "CREATE EXTENSION test_shmem;");

# Check that the attach counter is incremented on a new connection
my $attach_count1 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");
my $attach_count2 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");
cmp_ok($attach_count2, '>', $attach_count1,
	"attach callback is called in each backend");
$node->stop;

###
# Test that loading via shared_preload_libraries also works
###
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'test_shmem'");
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
