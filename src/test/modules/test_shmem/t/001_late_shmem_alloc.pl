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


$node->safe_psql("postgres", "CREATE EXTENSION test_shmem;");

# Run a registration twice in one backend.  The state under test is
# backend-local, so separate safe_psql calls would not exercise a retry.
sub register_twice
{
	my ($setup) = @_;
	my $stderr;

	$node->psql(
		"postgres", qq[
LOAD 'test_shmem';
$setup
DO \$\$
BEGIN
  FOR i IN 1..2 LOOP
    BEGIN
      PERFORM test_shmem_register();
      RAISE NOTICE 'attempt %: ok', i;
    EXCEPTION WHEN others THEN
      RAISE NOTICE 'attempt %: %', i, SQLERRM;
    END;
  END LOOP;
END
\$\$;],
		stderr => \$stderr);
	return $stderr;
}

SKIP:
{
	skip "injection points not supported by this build", 2
	  if $ENV{enable_injection_points} ne 'yes';

	$node->safe_psql("postgres", "CREATE EXTENSION injection_points;");
	my $stderr = register_twice(
		"SELECT injection_points_attach('test-shmem-request', 'error');");
	my @failures =
	  ($stderr =~ /attempt \d: error triggered for injection point/g);
	is(scalar @failures, 2,
		"request callback failure is reported on both attempts");
	unlike($stderr, qr/server closed the connection/,
		"request callback failure does not crash the retry");
	$node->safe_psql("postgres",
		"SELECT injection_points_detach('test-shmem-request');");
}

my $stderr = register_twice("SET test_shmem.area_size = '1GB';");
my @failures = ($stderr =~ /attempt \d: not enough shared memory/g);
is(scalar @failures, 2, "allocation failure is reported on both attempts");
unlike($stderr, qr/server closed the connection/,
	"allocation failure does not crash the retry");


# A partly allocated batch is rolled back from ShmemIndex, although its bytes
# cannot be reclaimed.  The same names can therefore be used by a smaller
# retry, and the failed attempt leaves no usable local handle.
my $result = $node->safe_psql("postgres", q[
LOAD 'test_shmem';
SET test_shmem.area_size = '1kB';
SET test_shmem.extra_size = '1GB';
DO $$ BEGIN PERFORM test_shmem_register(); EXCEPTION WHEN others THEN END $$;
SELECT test_shmem_area_is_null(),
			 (SELECT count(*)
					FROM pg_shmem_allocations
				 WHERE name IN ('test_shmem area 1024', 'test_shmem extra area'));]);
is($result, "t|0", "partial allocation is hidden and its names are reusable");

$result = $node->safe_psql("postgres", q[
LOAD 'test_shmem';
SET test_shmem.area_size = '1kB';
DO $$ BEGIN PERFORM test_shmem_register(); END $$;
SELECT get_test_shmem_attach_count();]);
is($result, '0', "a smaller request can reuse a rolled-back name");

SKIP:
{
	skip "injection points not supported by this build", 2
	  if $ENV{enable_injection_points} ne 'yes';

	$node->safe_psql("postgres", "CREATE EXTENSION IF NOT EXISTS injection_points;");
	$result = $node->safe_psql("postgres", q[
LOAD 'test_shmem';
DO $$ BEGIN PERFORM injection_points_attach('test-shmem-init', 'error'); END $$;
SET test_shmem.area_size = '2kB';
DO $$ BEGIN PERFORM test_shmem_register(); EXCEPTION WHEN others THEN END $$;
SELECT test_shmem_area_is_null(),
			 (SELECT count(*)
					FROM pg_shmem_allocations
				 WHERE name = 'test_shmem area 2048');]);
	is($result, "t|0", "an init callback failure is rolled back");
	$node->safe_psql("postgres",
		"SELECT injection_points_detach('test-shmem-init');");
	$result = $node->safe_psql("postgres", q[
LOAD 'test_shmem';
SET test_shmem.area_size = '2kB';
DO $$ BEGIN PERFORM test_shmem_register(); END $$;
SELECT get_test_shmem_attach_count();]);
	is($result, '0', "a name is reusable after an init callback failure");
}

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
