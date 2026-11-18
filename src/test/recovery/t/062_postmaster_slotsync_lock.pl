# Copyright (c) 2026, PostgreSQL Global Development Group

# The postmaster must not wait for LogicalDecodingControlLock when it checks
# whether it can start the slot sync worker. It has no PGPROC, so waiting for
# an LWLock raises PANIC.
#
# Hold the lock in the standby's startup process while it replays the record
# that turns logical decoding on, then make the postmaster check the slot sync
# parameters by opening a new connection.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# wal_level = replica, no logical slot yet, so logical decoding is off.
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

if (!$primary->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$primary->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('phys_slot')");
$primary->backup('backup');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
my $connstr = $primary->connstr;
$standby->append_conf(
	'postgresql.conf', qq(
sync_replication_slots = on
primary_slot_name = 'phys_slot'
primary_conninfo = '$connstr dbname=postgres'
hot_standby_feedback = on
));
$standby->start;
$primary->wait_for_replay_catchup($standby);

# The postmaster does not start the worker while decoding is off.
ok( $standby->wait_for_log(
		qr/replication slot synchronization requires "effective_wal_level" >= "logical" on the primary/
	),
	'slot sync worker not started while logical decoding is off');

# Open the wakeup session before the lock is held: new backends take the
# lock in shared mode at startup and would wait.
my $wakeup = $standby->background_psql('postgres');

$standby->safe_psql('postgres',
	"SELECT injection_points_attach('logical-decoding-enable-locked', 'wait')"
);

# The first logical slot turns logical decoding on. The startup process
# replays that change and stops while holding the lock exclusively.
$primary->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('lsub', 'test_decoding', false, false, true)"
);

# Poll from the open session: a new connection would wake the postmaster
# (the step under test) and would itself wait for the lock.
my $reached = 0;
foreach my $i (1 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
{
	$reached = $wakeup->query_safe(
		"SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'startup' AND wait_event = 'logical-decoding-enable-locked'"
	);
	last if $reached eq '1';
	usleep(100_000);
}
is($reached, '1', 'startup process waits while holding the lock');

# Wake the postmaster. It runs LaunchMissingBackgroundProcesses(), which
# checks the slot sync parameters. The connection itself waits for the lock,
# so do not wait for it to finish.
my $log_offset = -s $standby->logfile;
my $newconn = $standby->background_psql('postgres', wait => 0);
$newconn->query_until(qr//, "SELECT 1;\n");

# Give the postmaster time to act on the new connection.
sleep(2);

ok( !$standby->log_contains(
		qr/cannot wait without a PGPROC structure/, $log_offset),
	'postmaster did not PANIC while the lock was held');
is($wakeup->query_safe('SELECT 1'), '1', 'server still accepts queries');

$wakeup->query_safe(
	"SELECT injection_points_wakeup('logical-decoding-enable-locked')");
$wakeup->query_safe(
	"SELECT injection_points_detach('logical-decoding-enable-locked')");
$wakeup->quit;
$newconn->quit;

# Logical decoding is now on, so the slot sync worker starts and syncs the
# failover slot.
$standby->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_replication_slots WHERE slot_name = 'lsub' AND synced"
) or die 'timed out waiting for the slot to be synced';
pass('slot sync worker started after logical decoding was enabled');

$standby->stop;
$primary->stop;

done_testing();
