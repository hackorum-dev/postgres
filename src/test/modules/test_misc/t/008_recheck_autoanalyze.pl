# Copyright (c) 2025, PostgreSQL Global Development Group

# Tests rechecking whether an ANALYZE is required after an autovacuum
# performs only a VACUUM on a table, even if the table didn't initially
# require an ANALYZE when the VACUUM started.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Initialize postgres
my $psql_err = '';
my $psql_out = '';
my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;

$node->append_conf(
	'postgresql.conf', qq[
# This ensures a quick worker spawn.
autovacuum_naptime = 1s
log_autovacuum_min_duration = 0
]);
$node->start;

# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

# From this point, autovacuum worker will wait after vacuuming one heap relation.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('before-recheck-autoanalyze', 'wait');");

# Grab location in logs of primary
my $offset = -s $node->logfile;

# Create a table and insert 70 rows of data, which exceeds the value of
# autovacuum_vacuum_insert_threshold and doesn't exceeds the value of
# autovacuum_analyze_threshold.
$node->safe_psql(
	'postgres', qq[
    CREATE TABLE public.test (id int) WITH
    (autovacuum_vacuum_insert_threshold = 50, autovacuum_analyze_threshold = 100);
    INSERT INTO public.test SELECT generate_series(1, 70);
]);

# Wait until an autovacuum worker starts.
$node->wait_for_event('autovacuum worker', 'before-recheck-autoanalyze');

# And grab one of them.
my $av_pid = $node->safe_psql(
	'postgres',
    "SELECT pid FROM pg_stat_activity "
  . "WHERE backend_type = 'autovacuum worker' AND wait_event = 'before-recheck-autoanalyze';");

# insert data to exceeds the value of autovacuum_analyze_threshold
$node->safe_psql('postgres',
    "INSERT INTO public.test SELECT generate_series(1, 70);");

# Wakeup the injection point.
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('before-recheck-autoanalyze');");

# Wait for the autovacuum worker to exit before scanning the logs.
$node->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity "
	  . "WHERE pid = '$av_pid' AND backend_type = 'autovacuum worker';");

# Check that ANALYZE is executed.
ok($node->log_contains(
		qr/\[$av_pid\] LOG:  automatic analyze of table "postgres.public.test"/,
		$offset),
		"ANALYZE triggered by recheck after vacuum");

# Release injection point.
$node->safe_psql('postgres',
	"SELECT injection_points_detach('before-recheck-autoanalyze');");

$node->stop;
done_testing();
