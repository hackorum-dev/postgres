# Copyright (c) 2026, PostgreSQL Global Development Group

# Test validation of subscription and replication slot properties.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $publisher = PostgreSQL::Test::Cluster->new('publisher');
$publisher->init(allows_streaming => 'logical');
$publisher->start;

my $subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$subscriber->init;
$subscriber->start;

my $publisher_connstr = $publisher->connstr . ' dbname=postgres';

# Recreate a logical slot with the requested failover property.
sub recreate_slot
{
	my ($slot_name, $failover) = @_;

	$publisher->safe_psql(
		'postgres', qq[
		SELECT pg_drop_replication_slot('$slot_name')
		WHERE EXISTS (
			SELECT FROM pg_replication_slots WHERE slot_name = '$slot_name');
		SELECT pg_create_logical_replication_slot(
			'$slot_name', 'pgoutput', false, false, $failover);
	]);
}

# Wait until the apply worker has released a slot.
sub wait_for_slot_inactive
{
	my ($slot_name) = @_;

	$publisher->poll_query_until(
		'postgres', qq[
		SELECT active_pid IS NULL
		FROM pg_replication_slots
		WHERE slot_name = '$slot_name';
	]) or die "timed out waiting for slot $slot_name to become inactive";
}

$publisher->safe_psql(
	'postgres', q[
	CREATE TABLE property_tab (a integer PRIMARY KEY, payload text);
	CREATE PUBLICATION property_pub FOR TABLE property_tab;
]);

$subscriber->safe_psql(
	'postgres', q[
	CREATE TABLE property_tab (a integer PRIMARY KEY, payload text);
]);

# A failover-enabled subscription requires a failover-enabled slot.
recreate_slot('failover_slot', 'true');
$subscriber->safe_psql(
	'postgres', qq[
	CREATE SUBSCRIPTION failover_sub
	CONNECTION '$publisher_connstr application_name=failover_sub'
	PUBLICATION property_pub
	WITH (slot_name = 'failover_slot', create_slot = false,
		  copy_data = false, enabled = false, failover = true,
		  disable_on_error = true);
	ALTER SUBSCRIPTION failover_sub ENABLE;
]);

$publisher->poll_query_until(
	'postgres', q[
	SELECT active AND failover
	FROM pg_replication_slots
	WHERE slot_name = 'failover_slot';
]) or die 'timed out waiting for the failover slot to become active';

$subscriber->safe_psql('postgres', 'ALTER SUBSCRIPTION failover_sub DISABLE');
wait_for_slot_inactive('failover_slot');
recreate_slot('failover_slot', 'false');

my $subscriber_log_offset = -s $subscriber->logfile;
$subscriber->safe_psql('postgres', 'ALTER SUBSCRIPTION failover_sub ENABLE');
$subscriber->wait_for_log(
	qr/replication slot "failover_slot" has failover disabled/,
	$subscriber_log_offset);

$subscriber->poll_query_until(
	'postgres', q[
	SELECT NOT subenabled
	FROM pg_subscription
	WHERE subname = 'failover_sub';
]) or die 'timed out waiting for failover_sub to be disabled';

$subscriber->poll_query_until(
	'postgres', q[
	SELECT apply_error_count > 0
	FROM pg_stat_subscription_stats
	WHERE subname = 'failover_sub';
]) or die 'timed out waiting for the apply error to be counted';

wait_for_slot_inactive('failover_slot');
$subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION failover_sub');

# A subscription without failover enabled must reject a failover-enabled slot.
recreate_slot('extra_failover_slot', 'true');
$subscriber->safe_psql(
	'postgres', qq[
	CREATE SUBSCRIPTION extra_failover_sub
	CONNECTION '$publisher_connstr application_name=extra_failover_sub'
	PUBLICATION property_pub
	WITH (slot_name = 'extra_failover_slot', create_slot = false,
		  copy_data = false, enabled = false, failover = false,
		  disable_on_error = true);
]);

$subscriber_log_offset = -s $subscriber->logfile;
$subscriber->safe_psql('postgres',
	'ALTER SUBSCRIPTION extra_failover_sub ENABLE');
$subscriber->wait_for_log(
	qr/replication slot "extra_failover_slot" has failover enabled/,
	$subscriber_log_offset);

$subscriber->poll_query_until(
	'postgres', q[
	SELECT NOT subenabled
	FROM pg_subscription
	WHERE subname = 'extra_failover_sub';
]) or die 'timed out waiting for extra_failover_sub to be disabled';

$subscriber->poll_query_until(
	'postgres', q[
	SELECT apply_error_count > 0
	FROM pg_stat_subscription_stats
	WHERE subname = 'extra_failover_sub';
]) or die 'timed out waiting for the apply error to be counted';

is( $subscriber->safe_psql(
		'postgres', q[
		SELECT NOT subenabled AND apply_error_count > 0
		FROM pg_subscription
		JOIN pg_stat_subscription_stats
		  ON subid = pg_subscription.oid
		WHERE pg_subscription.subname = 'extra_failover_sub';
	]),
	't',
	'failover mismatch disabled the subscription and counted an apply error');

wait_for_slot_inactive('extra_failover_slot');
$subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION extra_failover_sub');

$subscriber->stop;
$publisher->stop;

done_testing();
