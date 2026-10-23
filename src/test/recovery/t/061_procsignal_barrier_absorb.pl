# Copyright (c) 2026, PostgreSQL Global Development Group

# Sessions that wait where they cannot run CHECK_FOR_INTERRUPTS() still absorb
# the ProcSignalBarriers that can be absorbed where an ERROR is not allowed (see
# BarrierTypeAbsorbableWithoutError()): a session waiting
# for synchronous replication after its commit, and one in COPY FROM STDIN
# waiting for more data from its client.  DROP DATABASE must not wait for them,
# and they must go on waiting.  A pending barrier whose processing can raise
# an ERROR, such as a data checksums change, is not absorbed there.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);
# A synchronous standby that never connects.  It is set before the server
# starts, so that it is in effect before the first commit; only the session
# that waits for synchronous replication requests it.
$node->append_conf(
	'postgresql.conf', qq(
synchronous_standby_names = 'nosuch'
synchronous_commit = local
));
$node->start;

$node->safe_psql('postgres', 'CREATE TABLE copy_target (a int)');
$node->safe_psql('postgres', 'CREATE DATABASE target1');
$node->safe_psql('postgres', 'CREATE DATABASE target2');

# Start a psql running $sql in the background, with application_name $appname.
sub start_psql
{
	my ($appname, $sql, $in, $out, $err) = @_;

	return IPC::Run::start(
		[
			'psql', '--no-psqlrc', '--quiet',
			'--dbname' => $node->connstr('postgres')
			  . " application_name=$appname",
			defined $sql ? ('--command' => $sql) : ()
		],
		'<' => $in,
		'>' => $out,
		'2>' => $err);
}

# Return the PID of the session $appname once its wait event is $wait_event.
sub session_pid
{
	my ($appname, $wait_event) = @_;

	$node->poll_query_until('postgres',
		"SELECT wait_event = '$wait_event' FROM pg_stat_activity WHERE application_name = '$appname'"
	) or die "timed out waiting for session \"$appname\"";
	return $node->safe_psql('postgres',
		"SELECT pid FROM pg_stat_activity WHERE application_name = '$appname'");
}

# DROP DATABASE $dbname must complete while session $pid keeps waiting.
sub check_drop_does_not_wait
{
	my ($pid, $dbname, $what, $wait_event) = @_;
	my $offset = -s $node->logfile;
	my $timed_out = 0;

	# With a timeout, so that a regression fails here instead of hanging.
	$node->psql(
		'postgres',
		"DROP DATABASE $dbname",
		timeout => $PostgreSQL::Test::Utils::timeout_default,
		timed_out => \$timed_out);
	ok(!$timed_out, "$what: DROP DATABASE did not wait for the session");
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*) FROM pg_database WHERE datname = '$dbname'"),
		'0',
		"$what: DROP DATABASE completed");
	is( $node->safe_psql(
			'postgres', "SELECT wait_event FROM pg_stat_activity WHERE pid = $pid"),
		$wait_event,
		"$what: session is still waiting");
	ok( !$node->log_contains(
			"still waiting for backend with PID $pid", $offset),
		"$what: no wait for the barrier was reported");
}

#
# A session waiting for a synchronous standby that is not connected, after its
# transaction has committed locally.
#
my $log_offset = -s $node->logfile;
my ($sr_in, $sr_out, $sr_err) = ('', '', '');
my $syncrep = start_psql('syncrep_waiter',
	'SET synchronous_commit = on; CREATE TABLE syncrep_target ()',
	\$sr_in, \$sr_out, \$sr_err);
my $syncrep_pid = session_pid('syncrep_waiter', 'SyncRep');
check_drop_does_not_wait($syncrep_pid, 'target1', 'synchronous replication',
	'SyncRep');

# Disabling data checksums emits barriers whose processing can raise an ERROR,
# so the waiting session must not absorb them: the operation waits for it.
if ($node->safe_psql('postgres', 'SHOW data_checksums') ne 'on')
{
	$node->safe_psql('postgres', 'SELECT pg_enable_data_checksums()');
	$node->poll_query_until('postgres', 'SHOW data_checksums', 'on')
	  or die 'timed out waiting for data checksums to be enabled';
}
my $offset = -s $node->logfile;
$node->safe_psql('postgres', 'SELECT pg_disable_data_checksums()');
ok( eval {
		$node->wait_for_log(
			qr/still waiting for backend with PID $syncrep_pid to accept ProcSignalBarrier/,
			$offset);
		1;
	},
	'data checksums: the operation waits for the session');
isnt($node->safe_psql('postgres', 'SHOW data_checksums'),
	'off', 'data checksums: not disabled while the session waits');
is( $node->safe_psql(
		'postgres',
		"SELECT wait_event FROM pg_stat_activity WHERE pid = $syncrep_pid"),
	'SyncRep',
	'data checksums: session is still waiting');

# Release the session: its commit is acknowledged, without a cancel, and the
# data checksums change goes through.
$node->safe_psql('postgres',
	"ALTER SYSTEM SET synchronous_standby_names = ''");
$node->reload;
$syncrep->finish;
ok( !$node->log_contains(
		'canceling the wait for synchronous replication', $log_offset),
	'synchronous replication: the wait was not canceled');
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_class WHERE relname = 'syncrep_target'"),
	'1',
	'synchronous replication: transaction completed');
ok($node->poll_query_until('postgres', 'SHOW data_checksums', 'off'),
	'data checksums: disabled once the session moved on');

#
# A COPY FROM STDIN whose client sent one row and then paused.
#
my ($copy_in, $copy_out, $copy_err) = ("COPY copy_target FROM STDIN;\n1\n", '', '');
my $copy = start_psql('copy_paused', undef, \$copy_in, \$copy_out, \$copy_err);
$copy->pump_nb;
$node->poll_query_until('postgres',
	"SELECT wait_event = 'ClientRead' AND state = 'active' FROM pg_stat_activity WHERE application_name = 'copy_paused'"
) or die 'timed out waiting for COPY FROM STDIN to start';
my $copy_pid = session_pid('copy_paused', 'ClientRead');
check_drop_does_not_wait($copy_pid, 'target2', 'COPY FROM STDIN',
	'ClientRead');
$copy_in .= "2\n\\.\n";
$copy->finish;
is($node->safe_psql('postgres', 'SELECT count(*) FROM copy_target'),
	'2', 'COPY FROM STDIN: completed');

$node->stop;
done_testing();
