
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Test replication statistics data in pg_stat_replication_slots
use strict;
use warnings;
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Test if statistics data in pg_stat_replication_slots is sane after drop replication
# slot and restart.

# Test set-up
my $node = PostgreSQL::Test::Cluster->new('test');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', 'synchronous_commit = on');
$node->start;

# Check that replication slot stats are expected.
sub test_slot_stats
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $expected, $msg) = @_;

	my $result = $node->safe_psql(
		'postgres', qq[
		SELECT slot_name, total_txns > 0 AS total_txn,
			   total_bytes > 0 AS total_bytes
			   FROM pg_stat_replication_slots
			   ORDER BY slot_name]);
	is($result, $expected, $msg);
}

# Create table.
$node->safe_psql('postgres', "CREATE TABLE test_repl_stat(col1 int)");

# Create replication slots.
$node->safe_psql(
	'postgres', qq[
	SELECT pg_create_logical_replication_slot('regression_slot1', 'test_decoding');
	SELECT pg_create_logical_replication_slot('regression_slot2', 'test_decoding');
	SELECT pg_create_logical_replication_slot('regression_slot3', 'test_decoding');
	SELECT pg_create_logical_replication_slot('regression_slot4', 'test_decoding');
]);

# Insert some data.
$node->safe_psql('postgres',
	"INSERT INTO test_repl_stat values(generate_series(1, 5));");

$node->safe_psql(
	'postgres', qq[
	SELECT data FROM pg_logical_slot_get_changes('regression_slot1', NULL,
	NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
	SELECT data FROM pg_logical_slot_get_changes('regression_slot2', NULL,
	NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
	SELECT data FROM pg_logical_slot_get_changes('regression_slot3', NULL,
	NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
	SELECT data FROM pg_logical_slot_get_changes('regression_slot4', NULL,
	NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
]);

# Wait for the statistics to be updated.
$node->poll_query_until(
	'postgres', qq[
	SELECT count(slot_name) >= 4 FROM pg_stat_replication_slots
	WHERE slot_name ~ 'regression_slot'
	AND total_txns > 0 AND total_bytes > 0;
]) or die "Timed out while waiting for statistics to be updated";

# Test to drop one of the replication slot and verify replication statistics data is
# fine after restart.
$node->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('regression_slot4')");

$node->stop;
$node->start;

# Verify statistics data present in pg_stat_replication_slots are sane after
# restart.
test_slot_stats(
	$node,
	qq(regression_slot1|t|t
regression_slot2|t|t
regression_slot3|t|t),
	'check replication statistics are updated');

# Test to remove one of the replication slots and adjust
# max_replication_slots accordingly to the number of slots. This leads
# to a mismatch between the number of slots present in the stats file and the
# number of stats present in shared memory. We verify
# replication statistics data is fine after restart.

$node->stop;
my $datadir           = $node->data_dir;
my $slot3_replslotdir = "$datadir/pg_replslot/regression_slot3";

rmtree($slot3_replslotdir);

$node->append_conf('postgresql.conf', 'max_replication_slots = 2');
$node->start;

# Verify statistics data present in pg_stat_replication_slots are sane after
# restart.
test_slot_stats(
	$node,
	qq(regression_slot1|t|t
regression_slot2|t|t),
	'check replication statistics after removing the slot file');

# cleanup
$node->safe_psql('postgres', "DROP TABLE test_repl_stat");
$node->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('regression_slot1')");
$node->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('regression_slot2')");

# shutdown
$node->stop;

# Test if resetting statistics data in pg_stat_replication_slots while decoding is
# ongoing works correctly.

# Start pg_recvlogical process and wait for it to become active.
sub start_pg_recvlogical
{
	my ($node, $slot_name, $create_slot) = @_;

	my @cmd = (
		'pg_recvlogical', '-S', "$slot_name", '-d',
		$node->connstr('postgres'),
		'--start', '--no-loop', '-f', '-');
	push @cmd, '--create-slot' if $create_slot;

	# start pg_recvlogical process.
	my $pg_recvlogical = IPC::Run::start(@cmd);

	# Wait for the replication slot to become active.
	$node->poll_query_until('postgres',
		"SELECT EXISTS (SELECT 1 FROM pg_replication_slots WHERE slot_name = '$slot_name' AND active_pid IS NOT NULL)"
	) or die "slot never became active";

	return $pg_recvlogical;
}

$node = PostgreSQL::Test::Cluster->new('test2');
$node->init(allows_streaming => 'logical');
$node->start;
$node->safe_psql('postgres', "CREATE TABLE test(i int)");

# Start pg_recvlogical process. The walsender process creates and acquires
# the replication slot.
my $slot_name = "regression_slot";
my $pg_recvlogical = start_pg_recvlogical($node, $slot_name, 1);

# Reset the replication slot statistics.
$node->safe_psql('postgres',
	"SELECT pg_stat_reset_replication_slot('$slot_name');");
my $result = $node->safe_psql('postgres',
	"SELECT stats_reset IS NOT NULL FROM pg_stat_replication_slots WHERE slot_name = '$slot_name'"
);
is($result, "t", "replication slot statistics are reset");

# Test if the walsender properly updates the statistics.
$node->safe_psql('postgres', "INSERT INTO test VALUES (1)");
$node->poll_query_until('postgres',
	"SELECT total_txns > 0 FROM pg_stat_replication_slots WHERE slot_name = '$slot_name'"
);

# Shutdown
$pg_recvlogical->kill_kill;
$node->stop;

# Remove the stats file and restart the server.
$datadir = $node->data_dir;
my $stats_file = "$datadir/pg_stat/pgstat.stat";
unlink($stats_file)
  or die "could not remove $stats_file";
$node->start;

# Start pg_recvlogical again.
$pg_recvlogical = start_pg_recvlogical($node, $slot_name, 0);

# Check if the replication slot statistics have been removed.
$result = $node->safe_psql('postgres',
	"SELECT stats_reset IS NULL FROM pg_stat_replication_slots WHERE slot_name = '$slot_name'"
);
is($result, "t", "replication slot statistics are removed");

# Test if the replication slot statistics continue to be accumulated even
# after statistics have been removed.
$node->safe_psql('postgres', "INSERT INTO test VALUES (1)");
$node->poll_query_until('postgres',
	"SELECT total_txns > 0 FROM pg_stat_replication_slots WHERE slot_name = '$slot_name'"
);

# Shutdown
$pg_recvlogical->kill_kill;
$node->stop;

done_testing();
