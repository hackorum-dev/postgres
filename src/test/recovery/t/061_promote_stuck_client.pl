
# Copyright (c) 2026, PostgreSQL Global Development Group

# Check that promotion of a standby that keeps logical decoding enabled is
# not held up by a backend blocked writing to a client that stopped reading.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Promotion normally takes well under a second.  Without the fix it never
# finishes while the client is stuck.
my $promote_timeout = int($PostgreSQL::Test::Utils::timeout_default / 6);

sub promote_with_stuck_client
{
	my ($standby, $name) = @_;

	is( $standby->safe_psql('postgres', 'SHOW effective_wal_level'),
		'logical', "$name: logical decoding is enabled on the standby");

	# psql writes the COPY data to a pipe that we never read, so the backend
	# fills its socket buffer and blocks.
	my ($in, $out, $err) = ('', '', '');
	my $writer = IPC::Run::start(
		[
			'psql', '--no-psqlrc',
			'--dbname' => $standby->connstr('postgres'),
			'--command' =>
			  "COPY (SELECT repeat('x', 1000) FROM generate_series(1, 1000000)) TO STDOUT"
		],
		'<' => \$in,
		'>' => \$out,
		'2>' => \$err);

	# Without this the test passes when the output fits in the buffers.
	$standby->poll_query_until('postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'ClientWrite'"
	) or die "timed out waiting for a backend to block in ClientWrite";

	is( $standby->safe_psql(
			'postgres', "SELECT pg_promote(true, $promote_timeout)"),
		't',
		"$name: promotion completes with a backend blocked on a write");

	$writer->kill_kill;
	$standby->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()');
}

# Logical decoding is enabled by replay, and the slot is created on the
# standby.
{
	my $primary = PostgreSQL::Test::Cluster->new('primary_a');
	$primary->init(allows_streaming => 1);
	$primary->start;
	$primary->backup('backup');

	my $standby = PostgreSQL::Test::Cluster->new('standby_a');
	$standby->init_from_backup($primary, 'backup', has_streaming => 1);
	$standby->start;

	$primary->safe_psql('postgres',
		"SELECT pg_create_logical_replication_slot('primary_slot', 'test_decoding')"
	);
	$primary->wait_for_replay_catchup($standby);
	$standby->create_logical_slot_on_standby($primary, 'standby_slot',
		'postgres');

	promote_with_stuck_client($standby, 'enabled by replay');

	$standby->stop;
	$primary->stop;
}

# Logical decoding is enabled when the standby starts, and the slot comes
# from slot synchronization.
{
	my $primary = PostgreSQL::Test::Cluster->new('primary_b');
	$primary->init(allows_streaming => 1);
	$primary->start;
	$primary->safe_psql('postgres',
		"SELECT pg_create_physical_replication_slot('sb1');"
		  . "SELECT pg_create_logical_replication_slot('keep', 'test_decoding');"
	);
	$primary->backup('backup');

	my $standby = PostgreSQL::Test::Cluster->new('standby_b');
	$standby->init_from_backup($primary, 'backup', has_streaming => 1);
	my $connstr = $primary->connstr . ' dbname=postgres';
	$standby->append_conf(
		'postgresql.conf', qq(
primary_slot_name = 'sb1'
hot_standby_feedback = on
primary_conninfo = '$connstr'
));
	$standby->start;
	$primary->wait_for_replay_catchup($standby);

	# Slot synchronization needs the catalog xmin held by the feedback.
	$primary->poll_query_until('postgres',
		"SELECT xmin IS NOT NULL OR catalog_xmin IS NOT NULL FROM pg_replication_slots WHERE slot_name = 'sb1'"
	);
	$primary->safe_psql('postgres',
		"SELECT pg_create_logical_replication_slot('fslot', 'test_decoding', false, false, true)"
	);
	$primary->wait_for_replay_catchup($standby);
	$standby->safe_psql('postgres', 'SELECT pg_sync_replication_slots()');
	is( $standby->safe_psql(
			'postgres',
			"SELECT synced FROM pg_replication_slots WHERE slot_name = 'fslot'"),
		't',
		'failover slot is synced to the standby');

	promote_with_stuck_client($standby, 'enabled at startup');

	$standby->stop;
	$primary->stop;
}

done_testing();
