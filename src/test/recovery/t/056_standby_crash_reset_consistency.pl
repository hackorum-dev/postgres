# Copyright (c) 2026, PostgreSQL Global Development Group

# Test hot standby consistency after a postmaster crash reset.
#
# When a child exits abnormally the postmaster resets and forks a replacement
# startup process, as restart_after_crash directs.  Recovery then restarts from
# the redo pointer in the control file, which can be a long way behind
# minRecoveryPoint: pages flushed by the previous startup process already hold
# changes that this pass has not replayed yet.
#
# The standby must therefore refuse read-only connections until replay reaches
# minRecoveryPoint again, just as it does on a normal start.  This test checks
# that it does, that the first client admitted afterwards sees correct data, and
# that the startup process announces consistency before connections are allowed.
#
# recovery_min_apply_delay is used to make the check reliable rather than
# timing-dependent.  recoveryApplyDelay() does not apply the delay until
# consistency has been reached, so on a correct standby the delay is inert and
# replay runs to minRecoveryPoint at full speed, whereas a standby that wrongly
# believes it is already consistent honours the delay and stops well short.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

# Table size.  Large enough that minRecoveryPoint advances well beyond the redo
# pointer in the control file, small enough that a correct standby replays that
# distance quickly.
my $rows = 20_000;

# Distance required between minRecoveryPoint and the redo pointer before the
# test is meaningful.
my $min_gap = 1024 * 1024;

# Number of full-table UPDATE rounds.  Repeated rounds matter: a client admitted
# before minRecoveryPoint gets a snapshot taken from the base backup checkpoint,
# so the newest tuple versions are not yet visible to it.  Pruning on the primary
# removes the versions those superseded, and the pages reach the standby's disk
# ahead of replay, so the rows are missing from the count(*) checked below.  With
# a single round the superseded versions survive on the page and the count looks
# correct even though the standby is inconsistent.
my $churn_rounds = 6;

# ----------------------------------------------------------------------------
# Primary
# ----------------------------------------------------------------------------
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', qq{
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 16GB
});
$primary->start;

$primary->safe_psql(
	'postgres', qq{
CREATE TABLE t (id int PRIMARY KEY, v text);
INSERT INTO t SELECT g, md5(g::text) FROM generate_series(1, $rows) g;
CHECKPOINT;
});

# ----------------------------------------------------------------------------
# Standby
# ----------------------------------------------------------------------------
$primary->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);

# A tiny shared_buffers keeps the startup process evicting and flushing pages,
# which is what advances minRecoveryPoint.  The checkpoint settings prevent a
# restartpoint, so the control file's redo pointer stays at the base backup
# checkpoint and post-crash redo restarts well behind minRecoveryPoint.
#
# restart_after_crash has to be turned back on since PostgreSQL::Test::Cluster->init
# disables it, and without it the postmaster would exit on the kill below rather
# than perform a crash reset.
$standby->append_conf(
	'postgresql.conf', q{
hot_standby = on
restart_after_crash = on
shared_buffers = 128kB
bgwriter_lru_maxpages = 0
checkpoint_timeout = 1h
max_wal_size = 16GB
});
$standby->start;

# A few small transactions immediately after the checkpoint that redo will
# restart from.
for my $i (1 .. 3)
{
	$primary->safe_psql('postgres', "UPDATE t SET v = v WHERE id = $i");
}

# ----------------------------------------------------------------------------
# Open the gap between minRecoveryPoint and the control file redo pointer.  No
# test runs until this succeeds, so that a machine unable to produce a gap skips
# rather than fails.
# ----------------------------------------------------------------------------
my ($min_recovery_point, $redo_ptr, $gap);

for my $round (1 .. 10)
{
	$primary->safe_psql('postgres', "UPDATE t SET v = md5(v || '$round')");
	$primary->wait_for_catchup($standby, 'replay');

	($min_recovery_point, $redo_ptr, $gap) = split /\|/, $standby->safe_psql(
		'postgres', q{
SELECT (SELECT min_recovery_end_lsn FROM pg_control_recovery()),
       (SELECT redo_lsn FROM pg_control_checkpoint()),
       (SELECT min_recovery_end_lsn FROM pg_control_recovery()) -
       (SELECT redo_lsn FROM pg_control_checkpoint())});

	note
	  "round $round: minRecoveryPoint $min_recovery_point, redo pointer $redo_ptr, gap $gap bytes";

	last if $round >= $churn_rounds && $gap >= $min_gap;
}

if ($gap < $min_gap)
{
	plan skip_all =>
	  "could not open a gap between minRecoveryPoint ($min_recovery_point) "
	  . "and the control file redo pointer ($redo_ptr): got $gap bytes, need $min_gap";
}

# Arm the apply delay now that the standby has caught up, so that it has no
# effect on the setup above.  The replacement startup process inherits it from
# the postmaster.
$standby->append_conf('postgresql.conf', 'recovery_min_apply_delay = 1h');
$standby->reload;

# ----------------------------------------------------------------------------
# Kill a live standby backend, which forces the crash reset and with it a
# replacement startup process.
#
# One reset, deliberately.  The postmaster clears its own record of consistency
# when the replacement startup process reports that recovery has started, so a
# second reset in the same server lifetime begins from different state and does
# not test the same thing.
# ----------------------------------------------------------------------------
my $log_offset = -s $standby->logfile;

my $victim = $standby->background_psql('postgres');
my $victim_pid = $victim->query_safe('SELECT pg_backend_pid()');
chomp $victim_pid;

PostgreSQL::Test::Utils::system_log('pg_ctl', 'kill', 'KILL', $victim_pid);

# The backend is gone, so psql exits with an error of its own; ignore it.
eval { $victim->quit };

# Wait for the replacement startup process to begin replaying.  From here on the
# standby must not accept a connection until replay >= minRecoveryPoint.
$standby->wait_for_log(qr/redo starts at/, $log_offset);

# ----------------------------------------------------------------------------
# Probe until the standby accepts a connection, then check what it served.
# ----------------------------------------------------------------------------
my ($replay_lsn, $past_min_recovery_point, $behind, $count);

my $deadline = time() + $PostgreSQL::Test::Utils::timeout_default;
while (time() < $deadline)
{
	my ($rc, $stdout, $stderr) = $standby->psql(
		'postgres', qq{
SELECT pg_last_wal_replay_lsn(),
       pg_last_wal_replay_lsn() >= '$min_recovery_point'::pg_lsn,
       '$min_recovery_point'::pg_lsn - pg_last_wal_replay_lsn(),
       (SELECT count(*) FROM t)}, on_error_stop => 0);

	if ($rc == 0 && $stdout ne '')
	{
		($replay_lsn, $past_min_recovery_point, $behind, $count) = split /\|/,
		  $stdout;
		last;
	}

	usleep(100_000);
}

ok(defined $replay_lsn,
	"standby accepted a read-only connection within the timeout")
  or BAIL_OUT("standby never accepted a connection after the crash reset");

note
  "first post-crash connection: replay $replay_lsn, minRecoveryPoint $min_recovery_point, $behind bytes short, count $count";

is($past_min_recovery_point, 't',
	"standby replayed up to minRecoveryPoint ($min_recovery_point) before "
	  . "accepting connections (replay was at $replay_lsn, $behind bytes short)"
);

# Only UPDATEs run after the initial load, so the row count never changes.  Any
# other answer means the scan read pages that replay has not caught up with.
is($count, $rows,
	"standby served correct row count to its first post-crash connection");

# Consistency has to be announced before the postmaster opens for connections,
# and in that order.
my $log = slurp_file($standby->logfile, $log_offset);

like(
	$log,
	qr/redo starts at .*consistent recovery state reached.*ready to accept read-only connections/s,
	"crash-reset startup reached consistency before connections were allowed"
);

done_testing();
