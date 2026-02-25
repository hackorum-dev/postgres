# Copyright (c) 2025-2026, PostgreSQL Global Development Group

# Test that PITR with recovery_target_action = 'pause' correctly activates
# hot standby at the recovery target even when the initial XLOG_RUNNING_XACTS
# record has subxid_overflow=true (SUBXIDS_MISSING).
#
# The bug: when the first XLOG_RUNNING_XACTS seen during recovery is
# overflowed, the standby enters STANDBY_SNAPSHOT_PENDING and hot standby
# never activates.  recoveryPausesHere() then bails out silently because
# LocalHotStandbyActive is false, causing unintended promotion instead of
# the requested pause.
#
# To trigger the bug the overflow transaction must be open at the time of
# the base backup's forced checkpoint, so that the very first
# XLOG_RUNNING_XACTS record the standby sees is overflowed.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(has_archiving => 1, allows_streaming => 1);
$node_primary->start;

# A table with a committed row that must be visible at the recovery target.
$node_primary->safe_psql('postgres',
	"CREATE TABLE t_committed (id int); INSERT INTO t_committed VALUES (42);");

# Scratch table used only to force subtransaction XID assignment inside
# gen_subxids.  Subtransaction XIDs are only assigned when the subtransaction
# writes; without an actual write the PGPROC subxid cache never fills and
# subxidStatus.overflowed is never set.
$node_primary->safe_psql('postgres', "CREATE TABLE _subxid_work (id int)");

# PL/pgSQL function that forces XID assignment for each subtransaction level
# via an INSERT, then recurses.  With n=70 there are 70 simultaneous
# open subtransactions, exceeding PGPROC_MAX_CACHED_SUBXIDS (64) and
# setting subxidStatus.overflowed on the backend's PGPROC entry.  The flag
# persists until the enclosing top-level transaction ends.
$node_primary->safe_psql('postgres', q{
	CREATE OR REPLACE FUNCTION gen_subxids(n int) RETURNS void
	LANGUAGE plpgsql AS $$
	BEGIN
		IF n <= 0 THEN RETURN; END IF;
		INSERT INTO _subxid_work VALUES (n);
		PERFORM gen_subxids(n - 1);
		RETURN;
		EXCEPTION WHEN OTHERS THEN RAISE;
	END;
	$$;
});

# Open a long-running top-level transaction and saturate its subxid cache
# BEFORE taking the base backup.  The backup forces a checkpoint via
# pg_basebackup --checkpoint=fast; because the overflow transaction is still
# open at that point, GetCurrentRunningTransactions() sees
# subxidStates[].overflowed = true and writes XLOG_RUNNING_XACTS with
# subxid_overflow=true.  This will be the FIRST XLOG_RUNNING_XACTS the
# standby replays, putting it immediately into STANDBY_SNAPSHOT_PENDING.
my $bg = $node_primary->background_psql('postgres');
$bg->query_safe("BEGIN");
$bg->query_safe("SELECT gen_subxids(70)");

# Take the base backup.  The background transaction (with overflowed subxid
# cache) remains open throughout.
$node_primary->backup('base_backup');

# Create the named restore point while the overflow transaction is still
# open.  No non-overflowed XLOG_RUNNING_XACTS can appear between the
# backup's forced checkpoint and this point, so the standby will be in
# STANDBY_SNAPSHOT_PENDING when it reaches this WAL record.
my $target = 'subxid_overflow_target';
$node_primary->safe_psql('postgres',
	"SELECT pg_create_restore_point('$target')");

# Commit the overflow transaction and insert a row that must NOT be
# visible at the recovery target.
$bg->query_safe("COMMIT");
$bg->quit;
$node_primary->safe_psql('postgres',
	"INSERT INTO t_committed VALUES (99)");

# Switch WAL to flush the archive, then wait for archiving to complete.
my $walfile = $node_primary->safe_psql('postgres',
	"SELECT pg_walfile_name(pg_switch_wal())");
$node_primary->poll_query_until('postgres',
	"SELECT '$walfile' <= last_archived_wal FROM pg_stat_archiver")
  or die "Timed out waiting for WAL archiving to complete";

# Set up a PITR standby targeting our named restore point with
# recovery_target_action = 'pause'.  The fix ensures that even though the
# standby starts in STANDBY_SNAPSHOT_PENDING, it transitions to
# STANDBY_SNAPSHOT_READY (activating hot standby) before the pause is
# attempted, so the pause is not silently bypassed.
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, 'base_backup',
	has_restoring => 1);
$node_standby->append_conf('postgresql.conf',
	"recovery_target_name = '$target'
recovery_target_action = 'pause'");
$node_standby->start;

# Wait until the standby reaches the recovery target and pauses.
# Without the fix the server promotes instead and this times out.
$node_standby->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "Timed out: recovery did not pause at target (was it promoted instead?)";

# Verify the standby is still in recovery (paused, not promoted).
is( $node_standby->safe_psql('postgres', "SELECT pg_is_in_recovery()"),
	't',
	"standby is paused in recovery at target despite initial subxid overflow");

is( $node_standby->safe_psql('postgres',
		"SELECT pg_get_wal_replay_pause_state()"),
	'paused',
	"recovery_target_action=pause honoured when subxid overflow was present");

# Hot standby queries must work: the committed row is visible, the row
# committed after the restore point is not.
is( $node_standby->safe_psql('postgres',
		"SELECT id FROM t_committed ORDER BY id"),
	'42',
	"only pre-target committed row is visible during pause");

# Resume to promote at the target LSN.  The row inserted AFTER the restore
# point is not applied: recovery_target_action=pause promotes exactly at the
# target, it does not continue WAL replay past it.
$node_standby->safe_psql('postgres', "SELECT pg_wal_replay_resume()");
$node_standby->poll_query_until('postgres', "SELECT NOT pg_is_in_recovery()")
  or die "Timed out waiting for standby to promote after resume";

is( $node_standby->safe_psql('postgres',
		"SELECT count(*) FROM t_committed"),
	'1',
	"promoted at recovery target: only pre-target row visible (WAL replay stops at target)");

done_testing();
