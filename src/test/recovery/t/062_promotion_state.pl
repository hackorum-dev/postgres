# Copyright (c) 2026, PostgreSQL Global Development Group

# Test how pg_stat_recovery reports standby promotion.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

# The extension may be absent in an installcheck run.
my $injection_points_enabled = ($ENV{enable_injection_points} // '') eq 'yes';
my $have_injection_points = $injection_points_enabled
  && $primary->check_extension('injection_points');
$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points')
  if $have_injection_points;

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->start;
$primary->wait_for_replay_catchup($standby);

my $state_query = q[
  SELECT pg_is_in_recovery(), promote_triggered, recovery_end_state,
         new_timeline
    FROM pg_stat_recovery];

is($standby->safe_psql('postgres', $state_query),
	't|f|replaying|', 'standby is replaying before promotion');
is( $standby->safe_psql(
		'postgres',
		'SET ROLE pg_read_all_data; SELECT count(*) FROM pg_stat_recovery'),
	'0',
	'recovery state requires pg_read_all_stats privileges');

SKIP:
{
	skip 'Injection points not supported by this build', 3
	  unless $injection_points_enabled;
	skip 'Extension injection_points not installed', 3
	  unless $have_injection_points;

	my $before_switch = 'promotion-before-timeline-switch';
	$standby->safe_psql('postgres',
		"SELECT injection_points_attach('$before_switch', 'wait')");
	$standby->safe_psql('postgres', 'SELECT pg_promote(false)');
	$standby->wait_for_event('startup', $before_switch);
	is($standby->safe_psql('postgres', $state_query),
		't|t|ending|', 'recovery is ending before timeline selection');

	# Simulate a crash before the timeline switch. Recovery resumes as a
	# standby and the state from the interrupted promotion is cleared.
	$standby->stop('immediate');
	$standby->start;
	$primary->wait_for_replay_catchup($standby);
	is($standby->safe_psql('postgres', $state_query),
		't|f|replaying|',
		'restart before the timeline switch remains a standby');

	my $after_switch = 'promotion-after-wal-segment-cleanup';
	$standby->safe_psql('postgres',
		"SELECT injection_points_attach('$after_switch', 'wait')");
	$standby->safe_psql('postgres', 'SELECT pg_promote(false)');
	$standby->wait_for_event('startup', $after_switch);
	is( $standby->safe_psql('postgres', $state_query),
		't|t|timeline switched|2',
		'timeline switched while still in recovery');
	$standby->safe_psql('postgres',
		"SELECT injection_points_wakeup('$after_switch')");
}

$standby->promote unless $have_injection_points;
ok( $standby->poll_query_until(
		'postgres', 'SELECT count(*) = 0 FROM pg_stat_recovery'),
	'no recovery state after promotion completes');

done_testing();
