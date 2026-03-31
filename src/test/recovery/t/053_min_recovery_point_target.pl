
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that minRecoveryPoint in pg_control is correctly advanced
# when recovering to a restore point with various recovery_target_action
# settings.  The bug being tested: when a CHECKPOINT record is replayed
# immediately before a no-op record like RESTORE_POINT, the shutdown
# restartpoint would only advance minRecoveryPoint to the CHECKPOINT end,
# not to the actual replay position.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

# Helper: parse minRecoveryPoint from pg_controldata output
sub get_min_recovery_point
{
	my $datadir = shift;
	my ($stdout, $stderr) = run_command([ 'pg_controldata', $datadir ]);
	my @control_data = split("\n", $stdout);
	foreach (@control_data)
	{
		if ($_ =~ /^Minimum recovery ending location:\s*(.*)$/mg)
		{
			return $1;
		}
	}
	die "No minRecoveryPoint in control file found\n";
}

# Initialize primary node with archiving
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(has_archiving => 1, allows_streaming => 1);
$node_primary->start;

# Take a backup before generating the WAL we want to recover
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

# Create two restore points:
# 1) "rp_no_ckpt" before any new CHECKPOINT
# 2) "rp_after_ckpt" immediately after a CHECKPOINT (triggers the bug)
#
# Capture the LSN returned by pg_create_restore_point() directly to
# avoid a race with background WAL activity (e.g. RUNNING_XACTS).
my $lsn_no_ckpt = $node_primary->safe_psql('postgres',
	"SELECT pg_create_restore_point('rp_no_ckpt');");

my $lsn_after_ckpt = $node_primary->safe_psql('postgres',
	"CHECKPOINT; SELECT pg_create_restore_point('rp_after_ckpt');");

# Force WAL switch to ensure the segment is archived
$node_primary->safe_psql('postgres', "SELECT pg_switch_wal()");
my $walfile = $node_primary->safe_psql('postgres',
	"SELECT pg_walfile_name('$lsn_no_ckpt')");
$node_primary->poll_query_until('postgres',
	"SELECT last_archived_wal >= '$walfile' FROM pg_stat_archiver")
  or die "Timed out while waiting for WAL archiving";

$node_primary->stop;

##
## Test 1: recovery_target_action = shutdown, with preceding CHECKPOINT
##
## This is the scenario that triggers the bug.  CreateRestartPoint
## processes the new CHECKPOINT and only advances minRecoveryPoint to
## the end of that CHECKPOINT record, missing the RESTORE_POINT that
## follows.
##
my $node_shutdown_ckpt = PostgreSQL::Test::Cluster->new('shutdown_ckpt');
$node_shutdown_ckpt->init_from_backup($node_primary, $backup_name,
	standby => 0,
	has_restoring => 1);
$node_shutdown_ckpt->append_conf('postgresql.conf', qq{
recovery_target_name = 'rp_after_ckpt'
recovery_target_action = 'shutdown'
});

# Use run_log + pg_ctl because the server shuts itself down after
# reaching the recovery target.
run_log(
	[
		'pg_ctl',
		'--pgdata' => $node_shutdown_ckpt->data_dir,
		'--log' => $node_shutdown_ckpt->logfile,
		'start',
	]);

foreach my $i (0 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
{
	last if !-f $node_shutdown_ckpt->data_dir . '/postmaster.pid';
	usleep(100_000);
}

my $logfile = slurp_file($node_shutdown_ckpt->logfile());
like(
	$logfile,
	qr/recovery stopping at restore point "rp_after_ckpt"/,
	'shutdown with checkpoint: recovery reached the restore point');

my $min_lsn = get_min_recovery_point($node_shutdown_ckpt->data_dir);
ok($min_lsn ge $lsn_after_ckpt,
	"shutdown with checkpoint: minRecoveryPoint ($min_lsn) >= restore point LSN ($lsn_after_ckpt)");

##
## Test 2: recovery_target_action = shutdown, without preceding CHECKPOINT
##
## The restore point is created before the CHECKPOINT, so there is no
## unprocessed checkpoint when recovery reaches the target.
## CreateRestartPoint calls UpdateMinRecoveryPoint() which reads the
## replay position from shared memory.  This path was already correct,
## but we test it for completeness.
##
my $node_shutdown_no_ckpt = PostgreSQL::Test::Cluster->new('shutdown_no_ckpt');
$node_shutdown_no_ckpt->init_from_backup($node_primary, $backup_name,
	standby => 0,
	has_restoring => 1);
$node_shutdown_no_ckpt->append_conf('postgresql.conf', qq{
recovery_target_name = 'rp_no_ckpt'
recovery_target_action = 'shutdown'
});

run_log(
	[
		'pg_ctl',
		'--pgdata' => $node_shutdown_no_ckpt->data_dir,
		'--log' => $node_shutdown_no_ckpt->logfile,
		'start',
	]);

foreach my $i (0 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
{
	last if !-f $node_shutdown_no_ckpt->data_dir . '/postmaster.pid';
	usleep(100_000);
}

$logfile = slurp_file($node_shutdown_no_ckpt->logfile());
like(
	$logfile,
	qr/recovery stopping at restore point "rp_no_ckpt"/,
	'shutdown without checkpoint: recovery reached the restore point');

$min_lsn = get_min_recovery_point($node_shutdown_no_ckpt->data_dir);
ok($min_lsn ge $lsn_no_ckpt,
	"shutdown without checkpoint: minRecoveryPoint ($min_lsn) >= restore point LSN ($lsn_no_ckpt)");

##
## Test 3: recovery_target_action = promote, with preceding CHECKPOINT
##
## After promotion, the server writes an end-of-recovery checkpoint and
## minRecoveryPoint is cleared, so we can only verify that recovery
## reached the target.
##
my $node_promote = PostgreSQL::Test::Cluster->new('promote_ckpt');
$node_promote->init_from_backup($node_primary, $backup_name,
	standby => 0,
	has_restoring => 1);
$node_promote->append_conf('postgresql.conf', qq{
recovery_target_name = 'rp_after_ckpt'
recovery_target_action = 'promote'
});

$node_promote->start;
$node_promote->poll_query_until('postgres',
	"SELECT pg_is_in_recovery() = 'f';")
  or die "Timed out while waiting for promotion";

$logfile = slurp_file($node_promote->logfile());
like(
	$logfile,
	qr/recovery stopping at restore point "rp_after_ckpt"/,
	'promote with checkpoint: recovery reached the restore point');

$node_promote->stop;

##
## Test 4: recovery_target_action = pause, with preceding CHECKPOINT
##
## While paused, the server is still in recovery.  After a clean
## shutdown, the checkpointer writes a shutdown restartpoint, so we
## can verify minRecoveryPoint offline.
##
my $node_pause = PostgreSQL::Test::Cluster->new('pause_ckpt');
$node_pause->init_from_backup($node_primary, $backup_name,
	standby => 0,
	has_restoring => 1);
$node_pause->append_conf('postgresql.conf', qq{
recovery_target_name = 'rp_after_ckpt'
recovery_target_action = 'pause'
});

$node_pause->start;

# Wait until recovery reaches the pause point
$node_pause->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused';")
  or die "Timed out while waiting for recovery to pause";

$logfile = slurp_file($node_pause->logfile());
like(
	$logfile,
	qr/recovery stopping at restore point "rp_after_ckpt"/,
	'pause with checkpoint: recovery reached the restore point');

# Shut down cleanly, which triggers a shutdown restartpoint
$node_pause->stop('fast');

$min_lsn = get_min_recovery_point($node_pause->data_dir);
ok($min_lsn ge $lsn_after_ckpt,
	"pause with checkpoint: minRecoveryPoint ($min_lsn) >= restore point LSN ($lsn_after_ckpt)");

done_testing();
