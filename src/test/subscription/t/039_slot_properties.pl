# Copyright (c) 2026, PostgreSQL Global Development Group

# Test validation of subscription and replication slot properties.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $publisher = PostgreSQL::Test::Cluster->new('publisher');
$publisher->init(allows_streaming => 'logical');
$publisher->append_conf('postgresql.conf',
	"max_prepared_transactions = 10\nlogical_decoding_work_mem = 64kB");
$publisher->start;

my $subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$subscriber->init;
$subscriber->append_conf('postgresql.conf',
		"max_prepared_transactions = 10\n"
	  . "max_worker_processes = 20\n"
	  . "max_logical_replication_workers = 10\n"
	  . "max_parallel_apply_workers_per_subscription = 4");
$subscriber->start;

my $publisher_connstr = $publisher->connstr . ' dbname=postgres';

# Recreate a logical slot with the requested properties.
sub recreate_slot
{
	my ($slot_name, $two_phase, $failover) = @_;

	$publisher->safe_psql(
		'postgres', qq[
		SELECT pg_drop_replication_slot('$slot_name')
		WHERE EXISTS (
			SELECT FROM pg_replication_slots WHERE slot_name = '$slot_name');
		SELECT pg_create_logical_replication_slot(
			'$slot_name', 'pgoutput', false, $two_phase, $failover);
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
	CREATE SEQUENCE local_seq;
	CREATE TABLE property_tab (
		a integer PRIMARY KEY,
		payload text,
		local_value bigint DEFAULT nextval('local_seq'));
]);

my $subscriber_log_offset;

# A pending two-phase subscription requires two-phase decoding to remain
# disabled until all table synchronization workers have finished.
recreate_slot('pending_slot', 'true', 'false');
$subscriber->safe_psql(
	'postgres', qq[
	CREATE SUBSCRIPTION pending_sub
	CONNECTION '$publisher_connstr application_name=pending_sub'
	PUBLICATION property_pub
	WITH (slot_name = 'pending_slot', create_slot = false,
		  copy_data = true, enabled = false, two_phase = true,
		  disable_on_error = true);
]);

is( $subscriber->safe_psql(
		'postgres', q[
		SELECT subtwophasestate = 'p' AND EXISTS (
			SELECT FROM pg_subscription_rel
			WHERE srsubid = pg_subscription.oid
			AND srsubstate <> 'r')
		FROM pg_subscription
		WHERE subname = 'pending_sub';
	]),
	't',
	'two-phase subscription is pending while table synchronization is incomplete'
);

$subscriber_log_offset = -s $subscriber->logfile;
$subscriber->safe_psql('postgres', 'ALTER SUBSCRIPTION pending_sub ENABLE');
$subscriber->wait_for_log(
	qr/replication slot "pending_slot" has two_phase enabled/,
	$subscriber_log_offset);

$subscriber->poll_query_until(
	'postgres', q[
	SELECT NOT subenabled
	FROM pg_subscription
	WHERE subname = 'pending_sub';
]) or die 'timed out waiting for pending_sub to be disabled';

is( $subscriber->safe_psql(
		'postgres', q[
		SELECT subtwophasestate = 'p' AND EXISTS (
			SELECT FROM pg_subscription_rel
			WHERE srsubid = pg_subscription.oid
			AND srsubstate <> 'r')
		FROM pg_subscription
		WHERE subname = 'pending_sub';
	]),
	't',
	'two-phase subscription remains pending after the slot mismatch');

wait_for_slot_inactive('pending_slot');
$subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION pending_sub');

# A subscription without two-phase replication must reject a slot that can
# send prepared transactions. In particular, it must do so before parallel
# apply can execute subscriber-side defaults.
recreate_slot('disabled_slot', 'true', 'false');
$subscriber->safe_psql(
	'postgres', qq[
	CREATE SUBSCRIPTION disabled_sub
	CONNECTION '$publisher_connstr application_name=disabled_sub'
	PUBLICATION property_pub
	WITH (slot_name = 'disabled_slot', create_slot = false,
		  copy_data = false, enabled = false, two_phase = false,
		  streaming = parallel, disable_on_error = true);
]);

$publisher->safe_psql(
	'postgres', q[
	BEGIN;
	INSERT INTO property_tab
	SELECT i, repeat(i::text, 1000)
	FROM generate_series(1, 5000) AS i;
	PREPARE TRANSACTION 'disabled_gid';
]);

$subscriber_log_offset = -s $subscriber->logfile;
$subscriber->safe_psql('postgres', 'ALTER SUBSCRIPTION disabled_sub ENABLE');
$subscriber->wait_for_log(
	qr/replication slot "disabled_slot" has two_phase enabled/,
	$subscriber_log_offset);

$subscriber->poll_query_until(
	'postgres', q[
	SELECT NOT subenabled
	FROM pg_subscription
	WHERE subname = 'disabled_sub';
]) or die 'timed out waiting for disabled_sub to be disabled';
$subscriber->poll_query_until(
	'postgres', q[
	SELECT apply_error_count > 0
	FROM pg_stat_subscription_stats
	WHERE subname = 'disabled_sub';
]) or die 'timed out waiting for the apply error to be counted';

is( $subscriber->safe_psql(
		'postgres', 'SELECT last_value, is_called FROM local_seq'),
	'1|f',
	'subscriber-side defaults did not run before the mismatch error');
is($subscriber->safe_psql('postgres', 'SELECT count(*) FROM property_tab'),
	'0', 'no prepared transaction changes were applied');

$publisher->safe_psql('postgres', "ROLLBACK PREPARED 'disabled_gid'");
wait_for_slot_inactive('disabled_slot');
$subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION disabled_sub');

# Once a subscription has enabled two-phase replication, it must reject a
# replacement slot without two-phase decoding enabled.
recreate_slot('enabled_slot', 'true', 'false');
$subscriber->safe_psql(
	'postgres', qq[
	CREATE SUBSCRIPTION enabled_sub
	CONNECTION '$publisher_connstr application_name=enabled_sub'
	PUBLICATION property_pub
	WITH (slot_name = 'enabled_slot', create_slot = false,
		  copy_data = false, enabled = false, two_phase = true,
		  disable_on_error = true);
	ALTER SUBSCRIPTION enabled_sub ENABLE;
]);

$subscriber->poll_query_until(
	'postgres', q[
	SELECT subtwophasestate = 'e'
	FROM pg_subscription
	WHERE subname = 'enabled_sub';
]) or die 'timed out waiting for two-phase replication to be enabled';

$subscriber->safe_psql('postgres', 'ALTER SUBSCRIPTION enabled_sub DISABLE');
wait_for_slot_inactive('enabled_slot');
recreate_slot('enabled_slot', 'false', 'false');

$subscriber_log_offset = -s $subscriber->logfile;
$subscriber->safe_psql('postgres', 'ALTER SUBSCRIPTION enabled_sub ENABLE');
$subscriber->wait_for_log(
	qr/replication slot "enabled_slot" has two_phase disabled/,
	$subscriber_log_offset);

$subscriber->poll_query_until(
	'postgres', q[
	SELECT NOT subenabled
	FROM pg_subscription
	WHERE subname = 'enabled_sub';
]) or die 'timed out waiting for enabled_sub to be disabled';
$subscriber->poll_query_until(
	'postgres', q[
	SELECT apply_error_count > 0
	FROM pg_stat_subscription_stats
	WHERE subname = 'enabled_sub';
]) or die 'timed out waiting for the apply error to be counted';

wait_for_slot_inactive('enabled_slot');
$subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION enabled_sub');

# A failover-enabled subscription requires a failover-enabled slot.
recreate_slot('failover_slot', 'false', 'true');
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
recreate_slot('failover_slot', 'false', 'false');

$subscriber_log_offset = -s $subscriber->logfile;
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
recreate_slot('extra_failover_slot', 'false', 'true');
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
