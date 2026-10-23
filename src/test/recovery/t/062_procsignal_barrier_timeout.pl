# Copyright (c) 2026, PostgreSQL Global Development Group

# Test procsignal_barrier_timeout.  Operations that wait for every process to
# absorb a ProcSignalBarrier must not wait forever behind a session that is
# blocked writing to a client that has stopped reading: after the timeout such
# a session is terminated.  Sessions that are merely idle, sleeping or reading
# slowly must not be, nor a session sending a large value to a client that
# keeps reading it, until the operation has waited ten times the timeout.
# Sessions waiting for synchronous replication or for more COPY FROM STDIN data
# absorb the barrier while they wait (see 061_procsignal_barrier_absorb.pl).  A session blocked sending an
# error message cannot be terminated; the operation says so and keeps waiting.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time usleep);

my $copy_query =
  "COPY (SELECT generate_series(1, 1000000000), repeat('x', 200)) TO STDOUT";
my $terminated_re =
  'because it has not accepted ProcSignalBarrier within \d+ ms';
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

# Open a connection by hand and send $sql as a simple query, without reading
# the reply.  psql hands a row over only once it has received all of it, so it
# cannot read a single large message slowly; this can, or not read it at all.
# $options are passed to the server as for PGOPTIONS.
sub start_raw_session
{
	my ($node, $appname, $sql, $options) = @_;
	my $user = $node->safe_psql('postgres', 'SELECT current_user');
	my $sock = $node->raw_connect();
	my $params =
	  "user\0$user\0database\0postgres\0application_name\0$appname\0";
	my ($type, $buf);

	$params .= "options\0$options\0" if defined $options;
	$params .= "\0";
	$sock->syswrite(pack('NN', 8 + length($params), 196608) . $params);
	# Read messages until ReadyForQuery
	do
	{
		$type = raw_read($sock, 1);
		$buf = raw_read($sock, 4);
		raw_read($sock, unpack('N', $buf) - 4);
	} while ($type ne 'Z');
	$sock->syswrite('Q' . pack('N', 5 + length($sql)) . $sql . "\0");
	return $sock;
}

# Read exactly $len bytes from $sock.
sub raw_read
{
	my ($sock, $len) = @_;
	my $buf = '';

	while (length($buf) < $len)
	{
		my $n = $sock->sysread($buf, $len - length($buf), length($buf));

		die "connection closed" if !$n;
	}
	return $buf;
}

# Run $sql in the background while reading from $sock, 8kB at a time with a
# pause in between, so that the session sends a large message slowly but
# steadily.  Returns the time $sql took, once it has completed and either
# $sock has received $until or the connection was closed.
sub run_while_reading_raw_slowly
{
	my ($node, $sock, $sql, $until) = @_;
	my $out = '';
	my $t0 = time;
	my $op = start_psql($node, 'postgres', 'op_raw', $sql, \$out);
	my ($op_elapsed, $tail, $closed) = (undef, '', 0);

	while (1)
	{
		if (!$closed && $tail ne $until)
		{
			my $buf = '';
			my $n = $sock->sysread($buf, 8192);

			$closed = 1 if !$n;
			$tail = substr($tail . $buf, -length($until)) if $n;
		}
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
		last if defined $op_elapsed && ($closed || $tail eq $until);
		die "timed out running \"$sql\"" if time - $t0 > $timeout;
		usleep(10_000) if !defined $op_elapsed;
	}
	return $op_elapsed;
}

# Check that the session $pid was terminated by the barrier timeout.
sub check_terminated
{
	my ($node, $pid, $log_offset, $what) = @_;

	ok( $node->log_contains(
			"requesting termination of process $pid $terminated_re",
			$log_offset),
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
$primary->safe_psql('postgres', 'CREATE DATABASE target7');
$primary->safe_psql('postgres', 'CREATE DATABASE target8');
$primary->safe_psql('postgres', 'CREATE DATABASE target9');

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
# A session blocked while it sends an error message to a client that has
# stopped reading holds off interrupts, so SIGTERM does not release it.  The
# operation says that the session has not exited and keeps waiting for it, as
# it would without the timeout, until the client goes away.
#
my $raw = start_raw_session($primary, 'stuck_error',
	"SELECT repeat('x', 10000000)::int",
	'-c log_min_messages=fatal');
$pid = session_pid($primary, 'stuck_error', 'ClientWrite');
wait_until_stuck($primary, $pid);
$log_offset = -s $primary->logfile;
my $err_dropper = $primary->background_psql('postgres');
$err_dropper->query_until(qr/dropping/,
	"\\echo dropping\nDROP DATABASE target7;\n");
$primary->wait_for_log(
	qr/requesting termination of process $pid $terminated_re/,
	$log_offset);
$primary->wait_for_log(
	qr/process $pid has not exited \d+ ms after its termination was requested/,
	$log_offset);
ok(session_alive($primary, $pid),
	'error message: blocked session has not exited');
is( $primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_database WHERE datname = 'target7'"),
	'1',
	'error message: DROP DATABASE is still waiting');
$raw->close;
$err_dropper->quit;
is( $primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_database WHERE datname = 'target7'"),
	'0',
	'error message: DROP DATABASE completed once the client went away');
ok( $primary->log_contains(
		qr/\[$pid\].*FATAL:  terminating connection due to administrator command/,
		$log_offset),
	'error message: blocked session exited');

#
# A session sending one large value to a client that keeps reading it slowly
# cannot absorb the barrier before the whole value is out either, but it keeps
# completing sends, so it is not terminated: the operation waits for the value
# to be out, as it would without the timeout.
#
$raw = start_raw_session($primary, 'slow_bigvalue',
	"SELECT repeat('x', 6000000)");
$pid = session_pid($primary, 'slow_bigvalue', 'ClientWrite');
$log_offset = -s $primary->logfile;
$elapsed = run_while_reading_raw_slowly($primary, $raw,
	'DROP DATABASE target8', "Z\0\0\0\5I");
is( $primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_database WHERE datname = 'target8'"),
	'0',
	'large value: DROP DATABASE completed');
cmp_ok($elapsed, '>=', 2,
	'large value: DROP DATABASE waited longer than the timeout');
ok(!$primary->log_contains('requesting termination', $log_offset),
	'large value: no session was terminated');
ok(session_alive($primary, $pid),
	'large value: slowly reading session survived');
$raw->close;

#
# But a session that is still sending is terminated anyway once the operation
# has waited ten times the timeout, so that a client cannot hold it up forever
# by reading just enough.
#
$raw = start_raw_session($primary, 'slower_bigvalue',
	"SELECT repeat('x', 40000000)");
$pid = session_pid($primary, 'slower_bigvalue', 'ClientWrite');
$log_offset = -s $primary->logfile;
$elapsed = run_while_reading_raw_slowly($primary, $raw,
	'DROP DATABASE target9', "Z\0\0\0\5I");
is( $primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_database WHERE datname = 'target9'"),
	'0',
	'still sending: DROP DATABASE completed');
cmp_ok($elapsed, '>=', 20,
	'still sending: DROP DATABASE waited ten times the timeout');
ok( $primary->log_contains(
		qr/requesting termination of process $pid $terminated_re.*\n.*It is still sending, but the wait has reached its limit/,
		$log_offset),
	'still sending: session was terminated at the limit');
ok($primary->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid"),
	'still sending: session is gone');
$raw->close;

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
ok(!$primary->log_contains('requesting termination', $log_offset),
	'negative control: no session was terminated');
ok( session_alive($primary, $idle_pid)
	  && session_alive($primary, $sleeper_pid)
	  && session_alive($primary, $slow_pid),
	'negative control: idle, sleeping and slowly reading sessions survived');
$idle->quit;
$sleeper->kill_kill;
$slow->kill_kill;

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
