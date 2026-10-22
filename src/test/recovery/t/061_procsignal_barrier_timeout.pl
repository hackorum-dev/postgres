# Copyright (c) 2026, PostgreSQL Global Development Group

# Test procsignal_barrier_timeout.  Operations that wait for every process to
# absorb a ProcSignalBarrier must not wait forever behind a session that is
# blocked writing to a client that has stopped reading: after the timeout such
# a session is terminated.  Sessions that are merely idle, sleeping or reading
# slowly must not be, nor sessions that wait for something other than their
# client reading, such as synchronous replication or more COPY FROM STDIN data.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time usleep);

my $copy_query =
  "COPY (SELECT generate_series(1, 1000000000), repeat('x', 200)) TO STDOUT";
my $terminated_re =
  'because it has not accepted ProcSignalBarrier within 2000 ms';
my $timeout = $PostgreSQL::Test::Utils::timeout_default;

# Start a psql running $sql, with its output captured but not read.
sub start_psql
{
	my ($node, $dbname, $appname, $sql, $out) = @_;
	my $err = '';

	return IPC::Run::start(
		[
			'psql', '--no-psqlrc', '--quiet',
			'--dbname' => $node->connstr($dbname)
			  . " application_name=$appname",
			'--command' => $sql
		],
		'>' => $out,
		'2>' => \$err);
}

# Return the PID of the session with the given application_name, once its
# wait_event is $wait_event (or once it exists, if $wait_event is undef).
sub session_pid
{
	my ($node, $appname, $wait_event) = @_;
	my $cond = defined $wait_event ? "wait_event = '$wait_event'" : 'true';

	$node->poll_query_until('postgres',
		"SELECT $cond FROM pg_stat_activity WHERE application_name = '$appname'"
	) or die "timed out waiting for session \"$appname\"";
	return $node->safe_psql('postgres',
		"SELECT pid FROM pg_stat_activity WHERE application_name = '$appname'");
}

# Start a COPY TO STDOUT whose output is never read.  psql blocks writing to
# its stdout and stops reading from the server, so the backend ends up blocked
# in a client write, where it cannot absorb a ProcSignalBarrier.
sub start_stuck_session
{
	my ($node, $dbname, $appname) = @_;
	my $out = '';
	my $h = start_psql($node, $dbname, $appname, $copy_query, \$out);
	my $pid = session_pid($node, $appname, 'ClientWrite');

	wait_until_stuck($node, $pid);
	return ($h, $pid);
}

# Wait until the session $pid, which shows ClientWrite, is blocked for good.  It
# can show ClientWrite while psql is still reading, and even once psql has
# stopped, a backend that is woken up can find room for one more send on a
# Unix-domain socket and run on to its next write, absorbing any pending
# barrier on the way.  So wake it up until it stays put:
# pg_log_backend_memory_contexts() makes it log once it reaches its next
# CHECK_FOR_INTERRUPTS(), which it only does if it was not blocked for good.
sub wait_until_stuck
{
	my ($node, $pid) = @_;

	for (my $i = 0;; $i++)
	{
		my $offset = -s $node->logfile;

		die "session $pid does not stay blocked" if $i >= 20;
		$node->safe_psql('postgres',
			"SELECT pg_log_backend_memory_contexts($pid)");
		usleep(500_000);
		last
		  unless $node->log_contains("logging memory contexts of PID $pid",
			$offset);
		$node->poll_query_until('postgres',
			"SELECT wait_event = 'ClientWrite' FROM pg_stat_activity WHERE pid = $pid"
		) or die "timed out waiting for session $pid to block again";
	}
}

sub session_alive
{
	my ($node, $pid) = @_;

	return $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_stat_activity WHERE pid = $pid") eq '1';
}

# Check that the session $pid was terminated by the barrier timeout.
sub check_terminated
{
	my ($node, $pid, $log_offset, $what) = @_;

	ok( $node->log_contains(
			"terminating process $pid $terminated_re", $log_offset),
		"$what: blocked session was terminated after the timeout");
	ok( $node->log_contains(
			qr/\[$pid\].*FATAL:  terminating connection due to administrator command/,
			$log_offset),
		"$what: blocked session exited");
	ok($node->poll_query_until('postgres',
			"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid"),
		"$what: blocked session is gone");
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1, no_data_checksums => 1);
$primary->append_conf('postgresql.conf', 'procsignal_barrier_timeout = 2s');
$primary->start;
$primary->safe_psql('postgres', 'CREATE DATABASE other');
$primary->safe_psql('postgres', 'CREATE DATABASE target1');
$primary->safe_psql('postgres', 'CREATE DATABASE target2');
$primary->safe_psql('postgres', 'CREATE DATABASE target3');
$primary->safe_psql('postgres', 'CREATE DATABASE target4');
$primary->safe_psql('postgres', 'CREATE DATABASE target5');
$primary->safe_psql('postgres', 'CREATE DATABASE target6');
$primary->safe_psql('postgres', 'CREATE TABLE copy_target (a int)');

my ($h, $pid, $log_offset, $start, $elapsed);

#
# DROP DATABASE waits for every process to close its files of the dropped
# database, including a session connected to another database.
#
($h, $pid) = start_stuck_session($primary, 'other', 'stuck_dropdb');
$log_offset = -s $primary->logfile;
$start = time;
$primary->safe_psql('postgres', 'DROP DATABASE target1',
	timeout => $timeout);
$elapsed = time - $start;
is( $primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_database WHERE datname = 'target1'"),
	'0',
	'DROP DATABASE: completed with a blocked session on another database');
cmp_ok($elapsed, '>=', 2, 'DROP DATABASE: waited for the timeout');
check_terminated($primary, $pid, $log_offset, 'DROP DATABASE');
$h->kill_kill;

#
# Enabling data checksums online.
#
($h, $pid) = start_stuck_session($primary, 'postgres', 'stuck_checksums');
$log_offset = -s $primary->logfile;
$primary->safe_psql('postgres', 'SELECT pg_enable_data_checksums()');
ok( $primary->poll_query_until(
		'postgres',
		"SELECT setting FROM pg_settings WHERE name = 'data_checksums'", 'on'),
	'pg_enable_data_checksums(): reached "on" with a blocked session');
check_terminated($primary, $pid, $log_offset, 'pg_enable_data_checksums()');
$h->kill_kill;

#
# A WAL sender blocked sending a base backup to a client that has stopped
# reading.
#
my ($bb_out, $bb_err) = ('', '');
$h = IPC::Run::start(
	[
		'pg_basebackup',
		'--pgdata' => '-',
		'--format' => 'tar',
		'--wal-method' => 'none',
		'--checkpoint' => 'fast',
		'--dbname' => $primary->connstr('postgres')
		  . ' application_name=stuck_walsender'
	],
	'>' => \$bb_out,
	'2>' => \$bb_err);
$pid = session_pid($primary, 'stuck_walsender', 'ClientWrite');
wait_until_stuck($primary, $pid);
is( $primary->safe_psql(
		'postgres', "SELECT backend_type FROM pg_stat_activity WHERE pid = $pid"),
	'walsender',
	'base backup is served by a WAL sender');
$log_offset = -s $primary->logfile;
$primary->safe_psql('postgres', 'DROP DATABASE target4',
	timeout => $timeout);
check_terminated($primary, $pid, $log_offset, 'WAL sender');
$h->kill_kill;

#
# Negative control: idle sessions, a session in pg_sleep() and a client that
# reads slowly but keeps reading are not terminated, and do not delay the
# operations.
#
my $idle = $primary->background_psql('postgres');
my $idle_pid = $idle->query_safe('SELECT pg_backend_pid()');
my $sleep_out = '';
my $sleeper = start_psql($primary, 'postgres', 'sleeper',
	'SELECT pg_sleep(600)', \$sleep_out);
my $sleeper_pid = session_pid($primary, 'sleeper', 'PgSleep');
my $slow_out = '';
my $slow = start_psql($primary, 'other', 'slow_reader', $copy_query,
	\$slow_out);
my $slow_pid = session_pid($primary, 'slow_reader', 'ClientWrite');

# Run $sql, reading a little from the slow client now and then until $sql has
# completed and the SQL condition $done holds.  The slow client must keep
# reading for as long as a barrier may be waiting for it:
# pg_disable_data_checksums() returns before the data checksums launcher
# emits and waits for its barriers.  Returns the time $sql took.
sub run_while_reading_slowly
{
	my ($sql, $done) = @_;
	my $out = '';
	my $t0 = time;
	my $op = start_psql($primary, 'postgres', 'op', $sql, \$out);
	my $op_elapsed;

	while (1)
	{
		$slow->pump_nb;
		$slow_out = '';
		if (!defined $op_elapsed)
		{
			if ($op->pumpable)
			{
				$op->pump_nb;
			}
			else
			{
				$op->finish;
				$op_elapsed = time - $t0;
			}
		}
		last
		  if defined $op_elapsed
		  && $primary->safe_psql('postgres', "SELECT $done") eq 't';
		die "timed out running \"$sql\"" if time - $t0 > $timeout;
		usleep(50_000);
	}
	return $op_elapsed;
}

$log_offset = -s $primary->logfile;
$elapsed = run_while_reading_slowly('DROP DATABASE target2',
	"NOT EXISTS (SELECT FROM pg_database WHERE datname = 'target2')");
is( $primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_database WHERE datname = 'target2'"),
	'0',
	'negative control: DROP DATABASE completed');
cmp_ok($elapsed, '<', 2,
	'negative control: DROP DATABASE did not wait for the timeout');
run_while_reading_slowly('SELECT pg_disable_data_checksums()',
	"(SELECT setting FROM pg_settings WHERE name = 'data_checksums') = 'off'");
pass('negative control: data checksums disabled');
ok(!$primary->log_contains('terminating process', $log_offset),
	'negative control: no session was terminated');
ok( session_alive($primary, $idle_pid)
	  && session_alive($primary, $sleeper_pid)
	  && session_alive($primary, $slow_pid),
	'negative control: idle, sleeping and slowly reading sessions survived');
$idle->quit;
$sleeper->kill_kill;
$slow->kill_kill;

#
# Sessions that cannot absorb the barrier while they wait, but are not blocked
# writing to their client, are not terminated either.  A session waiting for
# synchronous replication has already committed locally, and one in COPY FROM
# STDIN waits for its client to send more data.  The operation keeps waiting
# for them and completes once they move on.
#
sub check_not_terminated
{
	my ($pid, $dbname, $what, $release) = @_;
	my $offset = -s $primary->logfile;
	my $dropper = $primary->background_psql('postgres');

	$dropper->query_safe('SET synchronous_commit = local');
	$dropper->query_until(qr/dropping/,
		"\\echo dropping\nDROP DATABASE $dbname;\n");

	# Reported only once the timeout has expired and the session was left
	# alone.
	$primary->wait_for_log(
		qr/still waiting for backend with PID $pid to accept ProcSignalBarrier/,
		$offset);
	ok(session_alive($primary, $pid), "$what: session is not terminated");
	ok(!$primary->log_contains('terminating process', $offset),
		"$what: no session was terminated");
	is( $primary->safe_psql(
			'postgres',
			"SELECT count(*) FROM pg_database WHERE datname = '$dbname'"),
		'1',
		"$what: DROP DATABASE is still waiting");

	$release->();
	$dropper->quit;
	is( $primary->safe_psql(
			'postgres',
			"SELECT count(*) FROM pg_database WHERE datname = '$dbname'"),
		'0',
		"$what: DROP DATABASE completed once the session moved on");
}

# A session waiting for a synchronous standby that is not connected.
$primary->safe_psql('postgres',
	"ALTER SYSTEM SET synchronous_standby_names = 'nosuch'");
$primary->reload;
my $syncrep_out = '';
my $syncrep = start_psql($primary, 'postgres', 'syncrep_waiter',
	'CREATE TABLE syncrep_target ()', \$syncrep_out);
my $syncrep_pid = session_pid($primary, 'syncrep_waiter', 'SyncRep');
check_not_terminated(
	$syncrep_pid,
	'target5',
	'synchronous replication',
	sub {
		$primary->safe_psql('postgres',
			'ALTER SYSTEM RESET synchronous_standby_names');
		$primary->reload;
	});
$syncrep->finish;

# A COPY FROM STDIN whose client sent one row and then paused.
my ($copy_in, $copy_out, $copy_err) = ("COPY copy_target FROM STDIN;\n1\n", '', '');
my $copy = IPC::Run::start(
	[
		'psql', '--no-psqlrc', '--quiet',
		'--dbname' => $primary->connstr('postgres')
		  . ' application_name=copy_paused'
	],
	'<' => \$copy_in,
	'>' => \$copy_out,
	'2>' => \$copy_err);
$copy->pump_nb;
$primary->poll_query_until('postgres',
	"SELECT wait_event = 'ClientRead' AND state = 'active' FROM pg_stat_activity WHERE application_name = 'copy_paused'"
) or die 'timed out waiting for COPY FROM STDIN to start';
my $copy_pid = session_pid($primary, 'copy_paused');
check_not_terminated(
	$copy_pid,
	'target6',
	'COPY FROM STDIN',
	sub {
		$copy_in .= "2\n\\.\n";
		$copy->finish;
	});
is($primary->safe_psql('postgres', 'SELECT count(*) FROM copy_target'),
	'2', 'COPY FROM STDIN: completed');

#
# With procsignal_barrier_timeout = 0, the operation keeps waiting for the
# blocked session.
#
$primary->append_conf('postgresql.conf', 'procsignal_barrier_timeout = 0');
$primary->reload;
$primary->poll_query_until('postgres', 'SHOW procsignal_barrier_timeout',
	'0')
  or die 'timed out waiting for the reload';
($h, $pid) = start_stuck_session($primary, 'other', 'stuck_disabled');
$log_offset = -s $primary->logfile;
my $dropper = $primary->background_psql('postgres');
$dropper->query_until(qr/dropping/, "\\echo dropping\nDROP DATABASE target3;\n");
$primary->wait_for_log(
	qr/still waiting for backend with PID $pid to accept ProcSignalBarrier/,
	$log_offset);
ok(session_alive($primary, $pid),
	'timeout disabled: blocked session is not terminated');
is( $primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_database WHERE datname = 'target3'"),
	'1',
	'timeout disabled: DROP DATABASE is still waiting');
$primary->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
$dropper->quit;
is( $primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_database WHERE datname = 'target3'"),
	'0',
	'timeout disabled: DROP DATABASE completed once the session was terminated'
);
$h->kill_kill;
$primary->append_conf('postgresql.conf', 'procsignal_barrier_timeout = 2s');
$primary->reload;

#
# Standby: replay of the record that enables logical decoding with
# wal_level = replica, and the end of recovery when logical decoding is
# enabled, wait for every process to update its cached state.
#
$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->append_conf('postgresql.conf', 'procsignal_barrier_timeout = 2s');
$standby->start;
$primary->wait_for_replay_catchup($standby);

($h, $pid) = start_stuck_session($standby, 'postgres', 'stuck_replay');
$log_offset = -s $standby->logfile;
$primary->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('primary_slot', 'pgoutput')");
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', 'SHOW effective_wal_level'),
	'logical', 'replay: logical decoding enabled with a blocked session');
check_terminated($standby, $pid, $log_offset, 'replay');
$h->kill_kill;

$standby->create_logical_slot_on_standby($primary, 'standby_slot',
	'postgres');
$idle = $standby->background_psql('postgres');
$idle_pid = $idle->query_safe('SELECT pg_backend_pid()');
($h, $pid) = start_stuck_session($standby, 'postgres', 'stuck_promote');
$log_offset = -s $standby->logfile;
$standby->promote;
is($standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'),
	'f', 'promotion: completed with a blocked session');
check_terminated($standby, $pid, $log_offset, 'promotion');
ok(session_alive($standby, $idle_pid),
	'promotion: idle session survived');
$idle->quit;
$h->kill_kill;

done_testing();
