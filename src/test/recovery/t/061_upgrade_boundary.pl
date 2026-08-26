# Copyright (c) 2026, PostgreSQL Global Development Group

# Test marking a shutdown checkpoint as an upgrade boundary: the
# deliberate end of this major version's WAL, pending an in-place upgrade.
# The marking is visible in the control file and in the WAL itself, and a
# cluster simply restarted on the same binary cancels it.
#
# A standby replaying a boundary stops streaming and pauses recovery. From
# there it can be shut down (to swap binaries), resumed (if the upgrade was
# abandoned and the primary keeps going), or promoted.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

# Fetch one field from the control file
sub control_field
{
	my ($node, $field) = @_;

	my ($stdout, $stderr) =
	  run_command([ 'pg_controldata', $node->data_dir ]);
	die "$field not found in control file\n"
	  unless $stdout =~ /^\Q$field\E:\s*(.*)$/m;

	return $1;
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

# Request state round trip on the primary
is( $primary->safe_psql('postgres', 'SELECT pg_upgrade_boundary_requested()'),
	'f',
	'initially not active');
$primary->safe_psql('postgres', 'SELECT pg_request_upgrade_boundary()');
is( $primary->safe_psql('postgres', 'SELECT pg_upgrade_boundary_requested()'),
	't',
	'active after request');
$primary->safe_psql('postgres', 'SELECT pg_cancel_upgrade_boundary()');
is( $primary->safe_psql('postgres', 'SELECT pg_upgrade_boundary_requested()'),
	'f',
	'cancelled again');
$primary->safe_psql('postgres', 'SELECT pg_cancel_upgrade_boundary()');
is( $primary->safe_psql('postgres', 'SELECT pg_upgrade_boundary_requested()'),
	'f',
	'cancelling with nothing requested is a no-op');
is( $primary->safe_psql(
		'postgres', 'SELECT pg_upgrade_boundary_lsn() IS NULL'),
	't',
	'no boundary replayed on the primary');

# Only a shutdown checkpoint can act on a pending request.
$primary->safe_psql('postgres', 'SELECT pg_request_upgrade_boundary()');
$primary->safe_psql('postgres', 'CHECKPOINT');
is(control_field($primary, "Latest checkpoint's upgrade flag"),
	'none', 'online checkpoint is not marked as a boundary');
is( $primary->safe_psql('postgres', 'SELECT pg_upgrade_boundary_requested()'),
	't',
	'request stays active across an online checkpoint');
$primary->safe_psql('postgres', 'SELECT pg_cancel_upgrade_boundary()');

# Shutting down with a request pending marks the shutdown checkpoint, and
# the control file keeps the marking, so the next binary started on this
# data directory can see it.
$primary->safe_psql('postgres', 'SELECT pg_request_upgrade_boundary()');
$primary->stop('fast');
is(control_field($primary, "Latest checkpoint's upgrade flag"),
	'boundary', 'shutdown checkpoint is marked as a boundary');

# The flag is in the WAL itself, not just the control file.
command_like(
	[
		'pg_waldump', '-p', $primary->data_dir . '/pg_wal',
		'-s', control_field($primary, 'Latest checkpoint location'),
		'-n', '1'
	],
	qr/CHECKPOINT_SHUTDOWN .*upgrade flag boundary/,
	'pg_waldump shows the flag on the boundary checkpoint record');

# pg_resetwal discards the WAL after the boundary checkpoint and writes a
# new one in its place, so the marking must not carry over to it.
command_ok([ 'pg_resetwal', $primary->data_dir ], 'pg_resetwal succeeds');
is(control_field($primary, "Latest checkpoint's upgrade flag"),
	'none', 'pg_resetwal clears the boundary marking');
command_like(
	[
		'pg_waldump', '-p', $primary->data_dir . '/pg_wal',
		'-s', control_field($primary, 'Latest checkpoint location'),
		'-n', '1'
	],
	qr/CHECKPOINT_SHUTDOWN (?!.*upgrade flag)/,
	'checkpoint written by pg_resetwal is not marked');

# A boundary can be requested again on the reset cluster.
$primary->start;
$primary->safe_psql('postgres', 'SELECT pg_request_upgrade_boundary()');
$primary->stop('fast');
is(control_field($primary, "Latest checkpoint's upgrade flag"),
	'boundary', 'boundary re-established after pg_resetwal');

# Abandon the upgrade: starting the primary on the same binary cancels the
# boundary as a side effect of writing new WAL on this timeline.
$primary->start;
ok( $primary->log_contains(
		'database system was shut down at an upgrade boundary'),
	'primary logged the cancelled boundary');
is( $primary->safe_psql('postgres', 'SELECT pg_upgrade_boundary_requested()'),
	'f',
	'restarting does not re-request');

# No checkpoint has been written since, so a crash now makes recovery start
# at the boundary checkpoint record itself.  Crash recovery ignores the
# marking.
$primary->safe_psql('postgres',
	'CREATE TABLE crash (a int); INSERT INTO crash VALUES (1)');
$primary->stop('immediate');
$primary->start;
ok($primary->log_contains('automatic recovery in progress'),
	'primary went through crash recovery');
ok(!$primary->log_contains('reached an upgrade boundary'),
	'crash recovery ignored the boundary');
is($primary->safe_psql('postgres', 'SELECT count(*) FROM crash'),
	'1', 'crash recovery replayed past the boundary');

$primary->safe_psql('postgres', 'CHECKPOINT');
is(control_field($primary, "Latest checkpoint's upgrade flag"),
	'none', 'cancelled boundary is gone from the primary control file');

# Everything above needs only a primary.  Add a standby and walk it through
# a boundary. It must be streaming when the primary shuts down, since the
# boundary reaches it as WAL like anything else.
$primary->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->start;

# The request and cancel functions are refused during recovery
my ($ret, $stdout, $stderr) =
  $standby->psql('postgres', 'SELECT pg_request_upgrade_boundary()');
like(
	$stderr,
	qr/recovery is in progress/,
	'requesting is refused on a standby');
($ret, $stdout, $stderr) =
  $standby->psql('postgres', 'SELECT pg_cancel_upgrade_boundary()');
like(
	$stderr,
	qr/recovery is in progress/,
	'cancelling is refused on a standby');
is( $standby->safe_psql('postgres', 'SELECT pg_upgrade_boundary_requested()'),
	'f',
	'request state reads as false on a standby');
is( $standby->safe_psql(
		'postgres', 'SELECT pg_upgrade_boundary_lsn() IS NULL'),
	't',
	'no boundary replayed yet on the standby');

$primary->safe_psql('postgres',
	'CREATE TABLE t (a int); INSERT INTO t VALUES (1);');
$primary->wait_for_catchup($standby);

# Request a boundary and shut the primary down. The standby replays the
# marked checkpoint and pauses there.
$primary->safe_psql('postgres', 'SELECT pg_request_upgrade_boundary()');
$primary->stop('fast');
$standby->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "timed out waiting for standby to pause at upgrade boundary";
ok(1, 'standby paused after replaying the upgrade boundary');

my $boundary_lsn =
  $standby->safe_psql('postgres', 'SELECT pg_upgrade_boundary_lsn()');
isnt($boundary_lsn, '', 'pg_upgrade_boundary_lsn reports the boundary');
is( $standby->safe_psql(
		'postgres', 'SELECT count(*) FROM pg_stat_wal_receiver'),
	'0',
	'walreceiver was stopped at the boundary');
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'1', 'paused standby still serves reads');
ok($standby->log_contains('recovery has reached an upgrade boundary'),
	'standby logged the boundary');

# Abandon this upgrade too: restart the primary and resume the standby.
# Resuming continues replay; unlike a recovery target, it does not promote.
$primary->start;
$primary->safe_psql('postgres', 'INSERT INTO t VALUES (2)');
$standby->safe_psql('postgres', 'SELECT pg_wal_replay_resume()');
$primary->wait_for_catchup($standby);
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'2', 'standby resumed following the primary after abandoned upgrade');
is($standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'),
	't', 'resuming from a boundary did not promote');
is( $standby->safe_psql(
		'postgres', 'SELECT pg_upgrade_boundary_lsn() IS NULL'),
	't',
	'boundary location is cleared once replay resumes');

# A later manual pause is not a boundary pause.
$standby->safe_psql('postgres', 'SELECT pg_wal_replay_pause()');
$standby->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "timed out waiting for standby to pause manually";
is( $standby->safe_psql(
		'postgres', 'SELECT pg_upgrade_boundary_lsn() IS NULL'),
	't',
	'manual pause after a boundary is not reported as a boundary');
$standby->safe_psql('postgres', 'SELECT pg_wal_replay_resume()');

# The resumed standby still has the boundary checkpoint as its restartpoint
# until it replays the primary's next checkpoint.  Shutting down in that
# window must not pin the boundary in the control file, since the data is
# already past it, and restarting must not treat the superseded boundary as
# reached: the standby must start up and keep following.
$standby->stop('fast');
is(control_field($standby, "Latest checkpoint's upgrade flag"),
	'none', 'control file does not pin a boundary replay has moved past');
$standby->start;
is( $standby->safe_psql('postgres', 'SELECT pg_get_wal_replay_pause_state()'),
	'not paused',
	'standby restarted past a boundary does not pause');
$primary->safe_psql('postgres', 'INSERT INTO t VALUES (3)');
$primary->wait_for_catchup($standby);
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'3',
	'restarted standby follows the primary past the superseded boundary');

# Add a cascading standby fed by the first one.  It sees the boundary as
# the same checkpoint record, relayed by a standby that pauses on it, so it
# needs no coordination to pause itself.
my $cascade = PostgreSQL::Test::Cluster->new('cascade');
$standby->backup('bkp2');
$cascade->init_from_backup($standby, 'bkp2', has_streaming => 1);
$cascade->start;

# Request again and shut down: the standby pauses at a second, distinct
# boundary.  Make sure both are streaming first, so the boundary reaches
# them.
$primary->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_replication WHERE state = 'streaming'")
  or die "timed out waiting for standby to stream";
$standby->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_replication WHERE state = 'streaming'")
  or die "timed out waiting for cascading standby to stream";
$primary->safe_psql('postgres', 'SELECT pg_request_upgrade_boundary()');
$primary->stop('fast');
$standby->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "timed out waiting for standby to pause at second upgrade boundary";
my $boundary_lsn2 =
  $standby->safe_psql('postgres', 'SELECT pg_upgrade_boundary_lsn()');
isnt($boundary_lsn2, $boundary_lsn, 'second boundary has a new location');

$cascade->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "timed out waiting for cascading standby to pause";
is($cascade->safe_psql('postgres', 'SELECT pg_upgrade_boundary_lsn()'),
	$boundary_lsn2, 'cascading standby paused at the same boundary');
is( $cascade->safe_psql(
		'postgres', 'SELECT count(*) FROM pg_stat_wal_receiver'),
	'0',
	'cascading standby stopped its walreceiver');
$cascade->stop('fast');
is(control_field($cascade, "Latest checkpoint's upgrade flag"),
	'boundary', 'cascading standby control file records the boundary');

# Shutting the paused standby down pins the boundary in its own control
# file, where the next binary started on that data directory can see it.
$standby->stop('fast');
is( control_field($standby, 'Database cluster state'),
	'shut down in recovery',
	'paused standby shut down cleanly');
is(control_field($standby, "Latest checkpoint's upgrade flag"),
	'boundary', 'standby control file records the boundary');

# A restarted standby starts recovery from the boundary checkpoint itself
# and re-pauses before fetching any WAL.
$standby->start;
$standby->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "timed out waiting for restarted standby to re-pause";
is($standby->safe_psql('postgres', 'SELECT pg_upgrade_boundary_lsn()'),
	$boundary_lsn2, 'restarted standby re-paused at the same boundary');

# Without hot standby nobody could resume a pause, so recovery shuts down
# cleanly at the boundary instead.  The node exits on its own, so drive
# pg_ctl directly rather than through start(), which expects it to stay up.
$standby->stop('fast');
$standby->append_conf('postgresql.conf', 'hot_standby = off');
command_ok(
	[
		'pg_ctl',
		'--pgdata' => $standby->data_dir,
		'--log' => $standby->logfile,
		'start'
	],
	'pg_ctl start returns with hot standby off');
my $attempts = 10 * $PostgreSQL::Test::Utils::timeout_default;
while (-f $standby->data_dir . '/postmaster.pid' && $attempts-- > 0)
{
	usleep(100_000);
}
ok(!-f $standby->data_dir . '/postmaster.pid',
	'standby without hot standby shut down at the boundary');
ok($standby->log_contains('shutting down at upgrade boundary'),
	'standby logged the shutdown');
is( control_field($standby, 'Database cluster state'),
	'shut down in recovery',
	'shutdown at the boundary was clean');
is(control_field($standby, "Latest checkpoint's upgrade flag"),
	'boundary', 'control file still records the boundary');

# Back to hot standby for the last exit.
$standby->append_conf('postgresql.conf', 'hot_standby = on');
$standby->start;
$standby->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "timed out waiting for standby to re-pause with hot standby on";

# Promotion is the other valid exit: fail over onto the old major version.
$standby->promote;
$standby->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die "timed out waiting for standby promotion";
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'3', 'promoted standby has all data up to the boundary');

done_testing();
