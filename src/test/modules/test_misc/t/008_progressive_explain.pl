# Copyright (c) 2023-2025, PostgreSQL Global Development Group
#
# Test progressive explain
#
# We need to make sure pg_stat_progress_explain does not show rows for the local
# session, even if progressive explain is enabled. For other sessions pg_stat_progress_explain
# should contain data for a PID only if progressive_explain is enabled and a query
# is running. Data needs to be removed when query finishes (or gets cancelled).

use strict;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize node
my $node = PostgreSQL::Test::Cluster->new('progressive_explain');

$node->init;
# Configure progressive explain to be logged immediately
$node->append_conf('postgresql.conf', 'progressive_explain_interval = 0');
$node->start;

# Test for local session
sub test_local_session
{
	my $setting = $_[0];
	# Make sure local session does not appear in pg_stat_progress_explain
	my $count = $node->safe_psql(
		'postgres', qq[
	SET progressive_explain='$setting';
	SELECT count(*) from pg_stat_progress_explain WHERE pid = pg_backend_pid()
	]);

	ok($count == "0",
		"Session cannot see its own explain with progressive_explain set to ${setting}");
}

# Tests for peer session
sub test_peer_session
{
	my $setting = $_[0];
	my $ret;

	# Start a background session and get its PID
	my $background_psql = $node->background_psql(
		'postgres',
		on_error_stop => 0);

	my $pid = $background_psql->query_safe(
		qq[
		SELECT pg_backend_pid();
	]);

	# Configure progressive explain in background session and run a simple query
	# letting it finish
	$background_psql->query_safe(
		qq[
		SET progressive_explain='$setting';
		SELECT 1;
	]);

	# Check that pg_stat_progress_explain contains no row for the PID that finished
	# its query gracefully
	$ret = $node->safe_psql(
		'postgres', qq[
	SELECT count(*) FROM pg_stat_progress_explain where pid = $pid
	]);

	ok($ret == "0",
		"pg_stat_progress_explain empty for completed query with progressive_explain set to ${setting}");

	# Start query in background session and leave it running
	$background_psql->query_until(
		qr/start/, q(
	\echo start
	SELECT pg_sleep(600);
	));

	$ret = $node->safe_psql(
		'postgres', qq[
	SELECT count(*) FROM pg_stat_progress_explain where pid = $pid
	]);

	# If progressive_explain is disabled pg_stat_progress_explain should not contain
	# row for PID
	if ($setting eq 'off') {
		ok($ret == "0",
			"pg_stat_progress_explain empty for running query with progressive_explain set to ${setting}");
	}
	# 1 row for pid is expected for running query
	else {
		ok($ret == "1",
			"pg_stat_progress_explain with 1 row for running query with progressive_explain set to ${setting}");
	}

	# Terminate running query and make sure it is gone
	$node->safe_psql(
		'postgres', qq[
	SELECT pg_cancel_backend($pid);
	]);

	$node->poll_query_until(
		'postgres', qq[
		SELECT count(*) = 0 FROM pg_stat_activity
		WHERE pid = $pid and state = 'active'
	]);

	# Check again pg_stat_progress_explain and confirm that the existing row is
	# now gone
	$ret = $node->safe_psql(
		'postgres', qq[
	SELECT count(*) FROM pg_stat_progress_explain where pid = $pid
	]);

	ok($ret == "0",
		"pg_stat_progress_explain empty for canceled query with progressive_explain set to ${setting}");
}

# Run tests for the local session
test_local_session('off');
test_local_session('on');

# Run tests for peer session
test_peer_session('off');
test_peer_session('on');

$node->stop;
done_testing();
