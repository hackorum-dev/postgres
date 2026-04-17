
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test for replication slot limit
# Ensure that max_slot_wal_keep_size limits the number of WAL files to
# be kept by replication slots.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;
use Time::HiRes qw(usleep);

# Initialize primary node, setting wal-segsize to 1MB
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1, extra => ['--wal-segsize=1']);
$node_primary->append_conf(
	'postgresql.conf', qq(
min_wal_size = 2MB
max_wal_size = 4MB
log_checkpoints = yes
));
$node_primary->start;
$node_primary->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('rep1')");

# The slot state and remain should be null before the first connection
my $result = $node_primary->safe_psql('postgres',
	"SELECT restart_lsn IS NULL, wal_status is NULL, safe_wal_size is NULL FROM pg_replication_slots WHERE slot_name = 'rep1'"
);
is($result, "t|t|t", 'check the state of non-reserved slot is "unknown"');


# Take backup
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

# Create a standby linking to it using the replication slot
my $node_standby = PostgreSQL::Test::Cluster->new('standby_1');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->append_conf('postgresql.conf', "primary_slot_name = 'rep1'");

$node_standby->start;

# Wait until the primary has processed standby feedback and advanced the
# slot's restart_lsn.  For a physical slot, restart_lsn is updated from
# the standby's reported flush position, so this waits for the primary-side
# slot state that the following wal_status checks depend on.
$node_primary->wait_for_slot_catchup('rep1', 'restart',
	$node_primary->lsn('write'));

# Stop standby
$node_standby->stop;

# Preparation done, the slot is the state "reserved" now
$result = $node_primary->safe_psql('postgres',
	"SELECT wal_status, safe_wal_size IS NULL FROM pg_replication_slots WHERE slot_name = 'rep1'"
);
is($result, "reserved|t", 'check the catching-up state');

# Advance WAL by one segment (= 1MB) on primary
$node_primary->advance_wal(1);
$node_primary->safe_psql('postgres', "CHECKPOINT;");

# The slot is always "safe" when fitting max_wal_size
$result = $node_primary->safe_psql('postgres',
	"SELECT wal_status, safe_wal_size IS NULL FROM pg_replication_slots WHERE slot_name = 'rep1'"
);
is($result, "reserved|t",
	'check that it is safe if WAL fits in max_wal_size');

$node_primary->advance_wal(4);
$node_primary->safe_psql('postgres', "CHECKPOINT;");

# The slot is always "safe" when max_slot_wal_keep_size is not set
$result = $node_primary->safe_psql('postgres',
	"SELECT wal_status, safe_wal_size IS NULL FROM pg_replication_slots WHERE slot_name = 'rep1'"
);
is($result, "reserved|t", 'check that slot is working');

# The standby can reconnect to primary
$node_standby->start;

$node_primary->wait_for_slot_catchup('rep1', 'restart',
	$node_primary->lsn('write'));

$node_standby->stop;

# Set max_slot_wal_keep_size on primary
my $max_slot_wal_keep_size_mb = 6;
$node_primary->append_conf(
	'postgresql.conf', qq(
max_slot_wal_keep_size = ${max_slot_wal_keep_size_mb}MB
));
$node_primary->reload;

# The slot is in safe state.

$result = $node_primary->safe_psql('postgres',
	"SELECT wal_status FROM pg_replication_slots WHERE slot_name = 'rep1'");
is($result, "reserved", 'check that max_slot_wal_keep_size is working');

# Advance WAL again then checkpoint, reducing remain by 2 MB.
$node_primary->advance_wal(2);
$node_primary->safe_psql('postgres', "CHECKPOINT;");

# The slot is still working
$result = $node_primary->safe_psql('postgres',
	"SELECT wal_status FROM pg_replication_slots WHERE slot_name = 'rep1'");
is($result, "reserved",
	'check that slot remains reserved after advancing WAL');

# The standby can reconnect to primary
$node_standby->start;
$node_primary->wait_for_slot_catchup('rep1', 'restart',
	$node_primary->lsn('write'));
$node_standby->stop;

# wal_keep_size overrides max_slot_wal_keep_size
$result = $node_primary->safe_psql('postgres',
	"ALTER SYSTEM SET wal_keep_size to '8MB'; SELECT pg_reload_conf();");
# Advance WAL again, reducing remain by 6 MB.
$node_primary->advance_wal(6);
$result = $node_primary->safe_psql('postgres',
	"SELECT wal_status as remain FROM pg_replication_slots WHERE slot_name = 'rep1'"
);
is($result, "extended",
	'check that wal_keep_size overrides max_slot_wal_keep_size');
# restore wal_keep_size
$result = $node_primary->safe_psql('postgres',
	"ALTER SYSTEM SET wal_keep_size to 0; SELECT pg_reload_conf();");

# The standby can reconnect to primary
$node_standby->start;
$node_primary->wait_for_slot_catchup('rep1', 'restart',
	$node_primary->lsn('write'));
$node_standby->stop;

# Advance WAL again without checkpoint, reducing remain by 6 MB.
$node_primary->advance_wal(6);

# Slot gets into 'extended' state
$result = $node_primary->safe_psql('postgres',
	"SELECT wal_status FROM pg_replication_slots WHERE slot_name = 'rep1'");
is($result, "extended", 'check that the slot state changes to "extended"');

# do checkpoint so that the next checkpoint runs too early
$node_primary->safe_psql('postgres', "CHECKPOINT;");

# Advance WAL again without checkpoint; remain goes to 0.
$node_primary->advance_wal(1);

# Slot gets into 'unreserved' state and safe_wal_size is negative
$result = $node_primary->safe_psql('postgres',
	"SELECT wal_status, safe_wal_size <= 0 FROM pg_replication_slots WHERE slot_name = 'rep1'"
);
is($result, "unreserved|t",
	'check that the slot state changes to "unreserved"');

# The standby still can connect to primary before a checkpoint
$node_standby->start;

$node_primary->wait_for_slot_catchup('rep1', 'restart',
	$node_primary->lsn('write'));

$node_standby->stop;

ok( !$node_standby->log_contains(
		"requested WAL segment [0-9A-F]+ has already been removed"),
	'check that required WAL segments are still available');

# Create one checkpoint, to improve stability of the next steps
$node_primary->safe_psql('postgres', "CHECKPOINT;");

# Prevent other checkpoints from occurring while advancing WAL segments
$node_primary->safe_psql('postgres',
	"ALTER SYSTEM SET max_wal_size='40MB'; SELECT pg_reload_conf()");

# Advance WAL again. The slot loses the oldest segment by the next checkpoint
my $logstart = -s $node_primary->logfile;
$node_primary->advance_wal(7);

# Now create another checkpoint and wait until the WARNING is issued
$node_primary->safe_psql('postgres',
	'ALTER SYSTEM RESET max_wal_size; SELECT pg_reload_conf()');
$node_primary->safe_psql('postgres', "CHECKPOINT;");
my $invalidated = 0;
for (my $i = 0; $i < 10 * $PostgreSQL::Test::Utils::timeout_default; $i++)
{
	if ($node_primary->log_contains(
			'invalidating obsolete replication slot "rep1"', $logstart))
	{
		$invalidated = 1;
		last;
	}
	usleep(100_000);
}
ok($invalidated, 'check that slot invalidation has been logged');

$result = $node_primary->safe_psql(
	'postgres',
	qq[
	SELECT slot_name, active, restart_lsn IS NULL, wal_status, safe_wal_size
	FROM pg_replication_slots WHERE slot_name = 'rep1']);
is($result, "rep1|f|t|lost|",
	'check that the slot became inactive and the state "lost" persists');

# Wait until current checkpoint ends
my $checkpoint_ended = 0;
for (my $i = 0; $i < 10 * $PostgreSQL::Test::Utils::timeout_default; $i++)
{
	if ($node_primary->log_contains("checkpoint complete: ", $logstart))
	{
		$checkpoint_ended = 1;
		last;
	}
	usleep(100_000);
}
ok($checkpoint_ended, 'waited for checkpoint to end');

# The invalidated slot shouldn't keep the old-segment horizon back;
# see bug #17103: https://postgr.es/m/17103-004130e8f27782c9@postgresql.org
# Test for this by creating a new slot and comparing its restart LSN
# to the oldest existing file.
my $redoseg = $node_primary->safe_psql('postgres',
	"SELECT pg_walfile_name(lsn) FROM pg_create_physical_replication_slot('s2', true)"
);
my $oldestseg = $node_primary->safe_psql('postgres',
	"SELECT pg_ls_dir AS f FROM pg_ls_dir('pg_wal') WHERE pg_ls_dir ~ '^[0-9A-F]{24}\$' ORDER BY 1 LIMIT 1"
);
$node_primary->safe_psql('postgres',
	qq[SELECT pg_drop_replication_slot('s2')]);
is($oldestseg, $redoseg, "check that segments have been removed");

# The standby no longer can connect to the primary
$logstart = -s $node_standby->logfile;
$node_standby->start;

my $failed = 0;
for (my $i = 0; $i < 10 * $PostgreSQL::Test::Utils::timeout_default; $i++)
{
	if ($node_standby->log_contains(
			"This replication slot has been invalidated due to \"wal_removed\".",
			$logstart))
	{
		$failed = 1;
		last;
	}
	usleep(100_000);
}
ok($failed, 'check that replication has been broken');

$node_primary->stop;
$node_standby->stop;

my $node_primary2 = PostgreSQL::Test::Cluster->new('primary2');
$node_primary2->init(allows_streaming => 1);
$node_primary2->append_conf(
	'postgresql.conf', qq(
min_wal_size = 32MB
max_wal_size = 32MB
log_checkpoints = yes
));
$node_primary2->start;
$node_primary2->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('rep1')");
$backup_name = 'my_backup2';
$node_primary2->backup($backup_name);

$node_primary2->stop;
$node_primary2->append_conf(
	'postgresql.conf', qq(
max_slot_wal_keep_size = 0
));
$node_primary2->start;

$node_standby = PostgreSQL::Test::Cluster->new('standby_2');
$node_standby->init_from_backup($node_primary2, $backup_name,
	has_streaming => 1);
$node_standby->append_conf('postgresql.conf', "primary_slot_name = 'rep1'");
$node_standby->start;
$node_primary2->advance_wal(1);
$result = $node_primary2->safe_psql(
	'postgres',
	"CHECKPOINT; SELECT 'finished';",
	timeout => $PostgreSQL::Test::Utils::timeout_default);
is($result, 'finished', 'check if checkpoint command is not blocked');

$node_primary2->stop;
$node_standby->stop;

# The next test depends on Perl's `kill`, which apparently is not
# portable to Windows.  (It would be nice to use Test::More's `subtest`,
# but that's not in the ancient version we require.)
if ($PostgreSQL::Test::Utils::windows_os)
{
	done_testing();
	exit;
}

# Get a slot terminated while the walsender is active
# We do this by sending SIGSTOP to the walsender.  Skip this on Windows.
my $node_primary3 = PostgreSQL::Test::Cluster->new('primary3');
$node_primary3->init(allows_streaming => 1, extra => ['--wal-segsize=1']);
$node_primary3->append_conf(
	'postgresql.conf', qq(
	min_wal_size = 2MB
	max_wal_size = 2MB
	log_checkpoints = yes
	max_slot_wal_keep_size = 1MB
	));
$node_primary3->start;
$node_primary3->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('rep3')");
# Take backup
$backup_name = 'my_backup';
$node_primary3->backup($backup_name);
# Create standby
my $node_standby3 = PostgreSQL::Test::Cluster->new('standby_3');
$node_standby3->init_from_backup($node_primary3, $backup_name,
	has_streaming => 1);
$node_standby3->append_conf('postgresql.conf', "primary_slot_name = 'rep3'");
$node_standby3->start;
$node_primary3->wait_for_catchup($node_standby3);

my $senderpid;

# We've seen occasional cases where multiple walsender pids are still active
# at this point, apparently just due to process shutdown being slow. To avoid
# spurious failures, retry a couple times.
my $i = 0;
while (1)
{
	my ($stdout, $stderr);

	$senderpid = $node_primary3->safe_psql('postgres',
		"SELECT pid FROM pg_stat_activity WHERE backend_type = 'walsender'");

	last if $senderpid =~ qr/^[0-9]+$/;

	diag "multiple walsenders active in iteration $i";

	# show information about all active connections
	$node_primary3->psql(
		'postgres',
		"\\a\\t\nSELECT * FROM pg_stat_activity",
		stdout => \$stdout,
		stderr => \$stderr);
	diag $stdout, $stderr;

	if ($i++ == 10 * $PostgreSQL::Test::Utils::timeout_default)
	{
		# An immediate shutdown may hide evidence of a locking bug. If
		# retrying didn't resolve the issue, shut down in fast mode.
		$node_primary3->stop('fast');
		$node_standby3->stop('fast');
		die "could not determine walsender pid, can't continue";
	}

	usleep(100_000);
}

like($senderpid, qr/^[0-9]+$/, "have walsender pid $senderpid");

my $receiverpid = $node_standby3->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE backend_type = 'walreceiver'");
like($receiverpid, qr/^[0-9]+$/, "have walreceiver pid $receiverpid");

$logstart = -s $node_primary3->logfile;
# freeze walsender and walreceiver. Slot will still be active, but walreceiver
# won't get anything anymore.
kill 'STOP', $senderpid, $receiverpid;
$node_primary3->advance_wal(2);

my $msg_logged = 0;
my $max_attempts = $PostgreSQL::Test::Utils::timeout_default;
while ($max_attempts-- >= 0)
{
	if ($node_primary3->log_contains(
			"terminating process $senderpid to release replication slot \"rep3\"",
			$logstart))
	{
		$msg_logged = 1;
		last;
	}
	sleep 1;
}
ok($msg_logged, "walsender termination logged");

# Now let the walsender continue; slot should be killed now.
# (Must not let walreceiver run yet; otherwise the standby could start another
# one before the slot can be killed)
kill 'CONT', $senderpid;
$node_primary3->poll_query_until('postgres',
	"SELECT wal_status FROM pg_replication_slots WHERE slot_name = 'rep3'",
	"lost")
  or die "timed out waiting for slot to be lost";

$msg_logged = 0;
$max_attempts = $PostgreSQL::Test::Utils::timeout_default;
while ($max_attempts-- >= 0)
{
	if ($node_primary3->log_contains(
			'invalidating obsolete replication slot "rep3"', $logstart))
	{
		$msg_logged = 1;
		last;
	}
	sleep 1;
}
ok($msg_logged, "slot invalidation logged");

# Now let the walreceiver continue, so that the node can be stopped cleanly
kill 'CONT', $receiverpid;

$node_primary3->stop;
$node_standby3->stop;

# =============================================================================
# Testcase start: Check inactive_since property of the streaming standby's slot
#

# Initialize primary node
my $primary4 = PostgreSQL::Test::Cluster->new('primary4');
$primary4->init(allows_streaming => 'logical');
$primary4->start;

# Take backup
$backup_name = 'my_backup4';
$primary4->backup($backup_name);

# Create a standby linking to the primary using the replication slot
my $standby4 = PostgreSQL::Test::Cluster->new('standby4');
$standby4->init_from_backup($primary4, $backup_name, has_streaming => 1);

my $sb4_slot = 'sb4_slot';
$standby4->append_conf('postgresql.conf', "primary_slot_name = '$sb4_slot'");

my $slot_creation_time = $primary4->safe_psql(
	'postgres', qq[
    SELECT current_timestamp;
]);

$primary4->safe_psql(
	'postgres', qq[
    SELECT pg_create_physical_replication_slot(slot_name := '$sb4_slot');
]);

# Get inactive_since value after the slot's creation. Note that the slot is
# still inactive till it's used by the standby below.
my $inactive_since =
  $primary4->validate_slot_inactive_since($sb4_slot, $slot_creation_time);

$standby4->start;

# Wait until standby has replayed enough data
$primary4->wait_for_catchup($standby4);

# Now the slot is active so inactive_since value must be NULL
is( $primary4->safe_psql(
		'postgres',
		qq[SELECT inactive_since IS NULL FROM pg_replication_slots WHERE slot_name = '$sb4_slot';]
	),
	't',
	'last inactive time for an active physical slot is NULL');

# Stop the standby to check its inactive_since value is updated
$standby4->stop;

# Let's restart the primary so that the inactive_since is set upon loading the
# slot from the disk.
$primary4->restart;

is( $primary4->safe_psql(
		'postgres',
		qq[SELECT inactive_since > '$inactive_since'::timestamptz FROM pg_replication_slots WHERE slot_name = '$sb4_slot' AND inactive_since IS NOT NULL;]
	),
	't',
	'last inactive time for an inactive physical slot is updated correctly');

# Testcase end: Check inactive_since property of the streaming standby's slot
# =============================================================================

# =============================================================================
# Testcase start: Check inactive_since property of the logical subscriber's slot
my $publisher4 = $primary4;

# Create subscriber node
my $subscriber4 = PostgreSQL::Test::Cluster->new('subscriber4');
$subscriber4->init;

# Setup logical replication
my $publisher4_connstr = $publisher4->connstr . ' dbname=postgres';
$publisher4->safe_psql('postgres', "CREATE PUBLICATION pub FOR ALL TABLES");

$slot_creation_time = $publisher4->safe_psql(
	'postgres', qq[
    SELECT current_timestamp;
]);

my $lsub4_slot = 'lsub4_slot';
$publisher4->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot(slot_name := '$lsub4_slot', plugin := 'pgoutput');"
);

# Get inactive_since value after the slot's creation. Note that the slot is
# still inactive till it's used by the subscriber below.
$inactive_since =
  $publisher4->validate_slot_inactive_since($lsub4_slot, $slot_creation_time);

$subscriber4->start;
$subscriber4->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub CONNECTION '$publisher4_connstr' PUBLICATION pub WITH (slot_name = '$lsub4_slot', create_slot = false)"
);

# Wait until subscriber has caught up
$subscriber4->wait_for_subscription_sync($publisher4, 'sub');

# Now the slot is active so inactive_since value must be NULL
is( $publisher4->safe_psql(
		'postgres',
		qq[SELECT inactive_since IS NULL FROM pg_replication_slots WHERE slot_name = '$lsub4_slot';]
	),
	't',
	'last inactive time for an active logical slot is NULL');

# Stop the subscriber to check its inactive_since value is updated
$subscriber4->stop;

# Let's restart the publisher so that the inactive_since is set upon
# loading the slot from the disk.
$publisher4->restart;

is( $publisher4->safe_psql(
		'postgres',
		qq[SELECT inactive_since > '$inactive_since'::timestamptz FROM pg_replication_slots WHERE slot_name = '$lsub4_slot' AND inactive_since IS NOT NULL;]
	),
	't',
	'last inactive time for an inactive logical slot is updated correctly');

# Testcase end: Check inactive_since property of the logical subscriber's slot
# =============================================================================

$publisher4->stop;
$subscriber4->stop;

# Wait for the given slot to be invalidated with reason 'xid_aged'
sub wait_for_xid_aged_invalidation
{
	my ($node, $slot_name) = @_;
	$node->poll_query_until(
		'postgres', qq[
		SELECT COUNT(slot_name) = 1 FROM pg_replication_slots
			WHERE slot_name = '$slot_name' AND
			active = false AND
			invalidation_reason = 'xid_aged';
	]) or die "Timed out waiting for slot $slot_name to be invalidated";
}

# =====================================================================
# Testcase start: Invalidate physical slot due to max_slot_xid_age GUC

# Initialize primary node for XID age tests
my $primary5 = PostgreSQL::Test::Cluster->new('primary5');
$primary5->init(allows_streaming => 'logical');

# Disable autovacuum so checkpointer triggers the invalidation
my $max_slot_xid_age = 100;
$primary5->append_conf(
	'postgresql.conf', qq{
max_slot_xid_age = $max_slot_xid_age
autovacuum = off
});

$primary5->start;

# Create a procedure to consume XIDs
$primary5->safe_psql(
	'postgres', qq{
	CREATE PROCEDURE consume_xid(cnt int)
	AS \$\$
	DECLARE
	    i int;
	BEGIN
	    FOR i IN 1..cnt LOOP
	        EXECUTE 'SELECT pg_current_xact_id()';
	        COMMIT;
	    END LOOP;
	END;
	\$\$ LANGUAGE plpgsql;
});

# Take a backup for creating standby
$backup_name = 'backup5';
$primary5->backup($backup_name);

# Create standby with HS feedback so the slot gains an xmin
my $standby5 = PostgreSQL::Test::Cluster->new('standby5');
$standby5->init_from_backup($primary5, $backup_name, has_streaming => 1);
$standby5->append_conf(
	'postgresql.conf', q{
primary_slot_name = 'sb5_slot'
hot_standby_feedback = on
wal_receiver_status_interval = 1
});
$primary5->safe_psql(
	'postgres', qq[
    SELECT pg_create_physical_replication_slot(slot_name := 'sb5_slot', immediately_reserve := true);
]);
$standby5->start;

# Create some content on primary to move xmin
$primary5->safe_psql('postgres',
	"CREATE TABLE tab_int5 AS SELECT generate_series(1,10) AS a");
$primary5->wait_for_catchup($standby5);

# Wait for the physical slot to get xmin via hot_standby_feedback
$primary5->poll_query_until(
	'postgres', qq[
	SELECT xmin IS NOT NULL
		FROM pg_catalog.pg_replication_slots
		WHERE slot_name = 'sb5_slot';
]) or die "Timed out waiting for slot sb5_slot xmin from HS feedback";

# Stop standby so the slot becomes inactive with its xmin frozen
$standby5->stop;

# Advance XIDs past 2x max_slot_xid_age so the slot's xmin is stale enough
$primary5->safe_psql('postgres', qq{CALL consume_xid(2 * $max_slot_xid_age)});
$primary5->safe_psql('postgres', "CHECKPOINT");
wait_for_xid_aged_invalidation($primary5, 'sb5_slot');
ok(1, "physical slot invalidated due to XID age (via checkpoint)");

# Testcase end: Invalidate physical slot due to max_slot_xid_age GUC
# ===================================================================

# ====================================================================
# Testcase start: Invalidate logical slot due to max_slot_xid_age GUC

# Create a logical slot directly on the primary (no subscriber needed).
# The slot gets a catalog_xmin immediately upon creation.
$primary5->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('lsub5_slot', 'pgoutput')");

$primary5->poll_query_until(
	'postgres', qq[
	SELECT catalog_xmin IS NOT NULL
	FROM pg_catalog.pg_replication_slots
	WHERE slot_name = 'lsub5_slot';
]) or die "Timed out waiting for slot lsub5_slot catalog_xmin";

# Advance XIDs past 2x max_slot_xid_age so the slot's catalog_xmin is stale enough
$primary5->safe_psql('postgres', qq{CALL consume_xid(2 * $max_slot_xid_age)});

# Vacuum a user table so OldestXmin does not include the slot's catalog_xmin,
# skipping the invalidation of the slot.
$primary5->safe_psql('postgres', "VACUUM tab_int5");
is( $primary5->safe_psql(
		'postgres',
		qq[SELECT invalidation_reason IS NULL FROM pg_replication_slots WHERE slot_name = 'lsub5_slot';]
	),
	't',
	'logical slot not invalidated after vacuuming a data table');

# Vacuum a catalog table so OldestXmin includes the slot's catalog_xmin,
# triggering invalidation of the slot.
$primary5->safe_psql('postgres', "VACUUM pg_class");
wait_for_xid_aged_invalidation($primary5, 'lsub5_slot');
ok(1, "logical slot invalidated due to XID age (via vacuum)");

# Testcase end: Invalidate logical slot due to max_slot_xid_age GUC
# ==================================================================

# ===============================================================================
# Testcase start: Invalidate logical slot on standby due to max_slot_xid_age GUC

# Disable max_slot_xid_age on primary and recreate the streaming slot
$primary5->safe_psql(
	'postgres',
	q{
ALTER SYSTEM SET max_slot_xid_age = 0;
SELECT pg_reload_conf();
});
$primary5->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('sb5_slot')");
$primary5->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('sb5_slot', true)");
$standby5->append_conf(
	'postgresql.conf', qq{
max_slot_xid_age = $max_slot_xid_age
autovacuum = off
});
$standby5->start;

$primary5->wait_for_catchup($standby5);

$standby5->create_logical_slot_on_standby($primary5, 'sb5_logical_slot',
	'postgres');

$standby5->poll_query_until(
	'postgres', qq[
	SELECT catalog_xmin IS NOT NULL
	FROM pg_catalog.pg_replication_slots
	WHERE slot_name = 'sb5_logical_slot';
]) or die "Timed out waiting for sb5_logical_slot catalog_xmin";

# Advance XIDs on primary, replay on standby, then restartpoint to invalidate
$primary5->safe_psql('postgres', qq{CALL consume_xid(2 * $max_slot_xid_age)});
$primary5->safe_psql('postgres', "CHECKPOINT");
$primary5->wait_for_replay_catchup($standby5);
$standby5->safe_psql('postgres', "CHECKPOINT");

wait_for_xid_aged_invalidation($standby5, 'sb5_logical_slot');
ok(1, "logical (standby) slot invalidated due to XID age (via restartpoint)");

$standby5->stop;
$primary5->stop;

# Testcase end: Invalidate logical slot on standby due to max_slot_xid_age GUC
# =============================================================================

# =================================================================================
# Testcase start: XID-age-based slot invalidation with autovacuum (production-like)

# Standby sets slot xmin via HS feedback, disconnects, XIDs are consumed.
# max_slot_xid_age is set to vacuum_failsafe_age (1.6B) so autovacuum
# invalidates the slot before entering failsafe mode, unblocking
# datfrozenxid advancement and avoiding XID wraparound without manual
# VACUUM or downtime.

# Verify server log shows slot invalidation by autovacuum worker
sub verify_slot_xid_aged_invalidation_in_server_log
{
	my ($node, $slot_name, $max_age, $consumed_xids) = @_;

	my $log = slurp_file($node->logfile);

	# Verify the invalidation was performed by an autovacuum worker
	like($log,
		qr/autovacuum worker\[\d+\] LOG:\s+invalidating obsolete replication slot "$slot_name"/,
		"server log: $slot_name invalidated by autovacuum worker");

	# Verify DETAIL shows the xmin age exceeding max_slot_xid_age
	like($log,
		qr/autovacuum worker\[\d+\] DETAIL:\s+The slot's (?:catalog )?xmin age of (\d+) exceeds the configured "max_slot_xid_age" of $max_age by (\d+) transactions/,
		"server log: DETAIL shows xmin age exceeds max_slot_xid_age $max_age");

	# Extract xid age from the log and report for diagnostics
	$log =~
	  /The slot's (?:catalog )?xmin age of (\d+) exceeds the configured "max_slot_xid_age" of $max_age by (\d+)/;
	my $log_xid_age = $1 // 'N/A';
	my $exceeded_by = $2 // 'N/A';
	diag "xid_age from server log=$log_xid_age, exceeded_by=$exceeded_by, max_slot_xid_age=$max_age, consumed=$consumed_xids XIDs";
}

# Verify slot invalidation and wait for autovacuum to advance datfrozenxid
sub verify_invalidation_and_recovery
{
	my ($node, $slot_name, $max_age, $consumed_xids) = @_;

	return if $max_age == 0;

	wait_for_xid_aged_invalidation($node, $slot_name);
	ok(1, 'autovacuum invalidated slot due to xid_aged');

	verify_slot_xid_aged_invalidation_in_server_log($node, $slot_name,
		$max_age, $consumed_xids);

	# Wait for autovacuum to advance datfrozenxid in all databases past the
	# wraparound threshold.
	$node->poll_query_until(
		'postgres', qq[
		SELECT NOT EXISTS (
			SELECT 1 FROM pg_database
			WHERE age(datfrozenxid) > 2000000000
		);
	]) or die "Timed out waiting for autovacuum to advance datfrozenxid in all databases";
}

my $primary6 = PostgreSQL::Test::Cluster->new('primary6');
$primary6->init(allows_streaming => 'logical');

$max_slot_xid_age = 1600000000;    # matches vacuum_failsafe_age default
$primary6->append_conf(
	'postgresql.conf', qq{
max_slot_xid_age = $max_slot_xid_age
autovacuum_naptime = 1s
});

$primary6->start;
$primary6->safe_psql('postgres', "CREATE EXTENSION xid_wraparound");

$backup_name = 'backup6';
$primary6->backup($backup_name);

my $standby6 = PostgreSQL::Test::Cluster->new('standby6');
$standby6->init_from_backup($primary6, $backup_name, has_streaming => 1);
$standby6->append_conf(
	'postgresql.conf', q{
primary_slot_name = 'sb6_slot'
hot_standby_feedback = on
wal_receiver_status_interval = 1
});

$primary6->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('sb6_slot', true)");

$standby6->start;

$primary6->safe_psql('postgres',
	"CREATE TABLE tab_int6 AS SELECT generate_series(1,10) AS a");
$primary6->wait_for_catchup($standby6);

$primary6->poll_query_until(
	'postgres', qq[
	SELECT xmin IS NOT NULL FROM pg_replication_slots
		WHERE slot_name = 'sb6_slot';
]) or die "Timed out waiting for sb6_slot xmin from HS feedback";

# Stop standby; slot xmin persists and holds back datfrozenxid
$standby6->stop;

# Consume XIDs in 50M chunks; autovacuum (naptime=1s) will invalidate the
# slot once xmin age exceeds max_slot_xid_age.
my $logstart6 = -s $primary6->logfile;
my $chunk = 50_000_000;
my $max_xids = 2_200_000_000;
my $consumed = 0;

while ($consumed < $max_xids)
{
	$primary6->safe_psql('postgres', "SELECT consume_xids($chunk)");
	$consumed += $chunk;
	my $remaining = $max_xids - $consumed;
	diag "consumed $consumed / $max_xids XIDs ($remaining remaining)";
}

verify_invalidation_and_recovery($primary6, 'sb6_slot',
	$max_slot_xid_age, $consumed);

# Consume 1B more XIDs — combining with the 2.2B consumed above, the total
# of 3.2B exceeds the 2^31 (~2.1B) usable XID space (xidStopLimit), i.e.
# more than one full wraparound cycle, proving the system is healthy.
$primary6->safe_psql('postgres', "SELECT consume_xids(1000000000)");
ok(1, 'writes succeed after autovacuum invalidated the slot');

$primary6->stop;

# Testcase end: XID-age-based slot invalidation with autovacuum (production-like)
# ================================================================================

# ===============================================================================
# Testcase start: Concurrent slot invalidation due to max_slot_xid_age GUC
#
# Two concurrent VACUUMs both try to invalidate the same active logical slot.
# An injection point delays the walsender's SIGTERM processing so that vacuum1
# blocks on the CV waiting for the slot to be released.  When vacuum2 runs, it
# sees that vacuum1 is already invalidating the same slot and skips without
# blocking. After the walsender is woken, vacuum1 completes the invalidation.

# Skip if injection points are not available.
if ($ENV{enable_injection_points} ne 'yes')
{
	done_testing();
	exit;
}

my $primary7 = PostgreSQL::Test::Cluster->new('primary7');
$primary7->init(allows_streaming => 'logical');

my $max_slot_xid_age7 = 100;
$primary7->append_conf(
	'postgresql.conf', qq{
max_slot_xid_age = $max_slot_xid_age7
autovacuum = off
shared_preload_libraries = 'injection_points'
});

$primary7->start;

# Check if injection_points extension is available.
if (!$primary7->check_extension('injection_points'))
{
	$primary7->stop;
	done_testing();
	exit;
}

$primary7->safe_psql('postgres', 'CREATE EXTENSION injection_points');

# Helper to consume XIDs.
$primary7->safe_psql(
	'postgres', qq{
	CREATE PROCEDURE consume_xid(cnt int)
	AS \$\$
	DECLARE
	    i int;
	BEGIN
	    FOR i IN 1..cnt LOOP
	        EXECUTE 'SELECT pg_current_xact_id()';
	        COMMIT;
	    END LOOP;
	END;
	\$\$ LANGUAGE plpgsql;
});


# Create a logical slot (gets catalog_xmin immediately).
$primary7->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('lslot7', 'test_decoding')");

# Hold the slot active via pg_recvlogical.
my $pg_recvlog_stdout7 = '';
my $pg_recvlog_stderr7 = '';
my $connstr7 = $primary7->connstr('postgres');
my $pg_recvlogical_handle7 = IPC::Run::start(
	[
		'pg_recvlogical', '-d', $connstr7,
		'--slot', 'lslot7', '--start',
		'-f', '/dev/null', '--no-loop'
	],
	'>', \$pg_recvlog_stdout7,
	'2>', \$pg_recvlog_stderr7);

# Wait for the slot to become active.
$primary7->poll_query_until(
	'postgres', qq[
	SELECT active FROM pg_replication_slots WHERE slot_name = 'lslot7';
]) or die "Timed out waiting for slot lslot7 to become active";

# Make the walsender block before processing SIGTERM.
$primary7->safe_psql('postgres',
	"SELECT injection_points_attach('walsender-before-sigterm-exit', 'wait')");

# Make the slot's catalog_xmin stale.
$primary7->safe_psql('postgres',
	qq{CALL consume_xid(2 * $max_slot_xid_age7)});

# Launch vacuum1 on a catalog table: the logical slot holds catalog_xmin,
# so only catalog VACUUMs see it as OldestXmin and trigger invalidation.
# vacuum1 will SIGTERM the walsender, then block on the CV.
my $vacuum1 = $primary7->background_psql('postgres');
my $vacuum2 = $primary7->background_psql('postgres');

$vacuum1->query_until(
	qr/starting_vacuum/,
	q(\echo starting_vacuum
VACUUM pg_class;
\echo vacuum1_done
));

# Wait for the walsender to hit the injection point.
$primary7->wait_for_event('walsender', 'walsender-before-sigterm-exit');

# Verify vacuum1 is blocked on the CV.
$primary7->poll_query_until(
	'postgres', qq[
	SELECT count(*) = 1 FROM pg_stat_activity
		WHERE wait_event = 'ReplicationSlotDrop'
		AND backend_type = 'client backend';
]) or die "Timed out waiting for vacuum1 to block on slot CV";

# Launch vacuum2 on a different catalog table: it also computes the catalog
# horizon and sees the slot needs invalidation, but finds vacuum1 is already
# invalidating the same slot (via invalidating_proc) and skips.
$vacuum2->query_until(
	qr/starting_vacuum/,
	q(\echo starting_vacuum
VACUUM pg_type;
\echo vacuum2_done
));

# vacuum2 completes without blocking.
$vacuum2->query_until(qr/vacuum2_done/, '');

# Verify only vacuum1 is waiting on ReplicationSlotDrop.
$result = $primary7->safe_psql(
	'postgres', qq[
	SELECT count(*) FROM pg_stat_activity
		WHERE wait_event = 'ReplicationSlotDrop'
		AND backend_type = 'client backend';
]);
is($result, '1',
	'only vacuum1 blocks on CV; vacuum2 skips via invalidating_proc');

# Wake up the walsender so it can exit and release the slot.
$primary7->safe_psql('postgres',
	"SELECT injection_points_wakeup('walsender-before-sigterm-exit')");

# Detach the injection point.
$primary7->safe_psql('postgres',
	"SELECT injection_points_detach('walsender-before-sigterm-exit')");

# Wait for pg_recvlogical to exit.
$pg_recvlogical_handle7->finish;

# Wait for vacuum1 to complete now that the walsender has released the slot.
$vacuum1->query_until(qr/vacuum1_done/, '');

# Verify the slot was invalidated.
wait_for_xid_aged_invalidation($primary7, 'lslot7');
ok(1,
	"concurrent VACUUM: vacuum1 blocks on CV, vacuum2 skips via invalidating_proc"
);

$vacuum1->quit;
$vacuum2->quit;
$primary7->stop;

# Testcase end: Concurrent slot invalidation due to max_slot_xid_age GUC
# ===============================================================================

done_testing();
