
# Copyright (c) 2026, PostgreSQL Global Development Group

# Check that a backend blocked on a client write or read still absorbs
# ProcSignalBarriers, so that a stuck client cannot hold up promotion or
# DROP DATABASE.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;
$primary->safe_psql('postgres',
	'CREATE TABLE copy_target (a int); CREATE DATABASE dropme;');
$primary->backup('backup');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->start;

# A client that stops reading.  psql fetches the result in chunks and blocks
# writing to a pipe that we never read, so the backend fills its socket
# buffer and blocks in ClientWrite.
my ($in, $out, $err) = ('', '', '');
my $writer = IPC::Run::start(
	[
		'psql', '--no-psqlrc',
		'--set' => 'FETCH_COUNT=100',
		'--dbname' => $standby->connstr('postgres'),
		'--command' =>
		  "SELECT repeat('x', 1000) FROM generate_series(1, 1000000)"
	],
	'<' => \$in,
	'>' => \$out,
	'2>' => \$err);

$standby->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'ClientWrite'"
) or die "timed out waiting for a backend to block in ClientWrite";

# Promotion waits for every process to absorb a barrier.
my $promoted = $standby->safe_psql('postgres',
	"SELECT pg_promote(true, $PostgreSQL::Test::Utils::timeout_default)");
is($promoted, 't', 'promotion completes with a backend blocked on a write');

$writer->kill_kill;

# A client that stops sending.  COPY FROM STDIN waits for data on psql's
# stdin, which we never write to, so the backend blocks in ClientRead.
my ($in2, $out2, $err2) = ('', '', '');
my $reader = IPC::Run::start(
	[
		'psql', '--no-psqlrc',
		'--dbname' => $primary->connstr('postgres'),
		'--command' => 'COPY copy_target FROM STDIN'
	],
	'<' => \$in2,
	'>' => \$out2,
	'2>' => \$err2);

$primary->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'ClientRead' AND state = 'active'"
) or die "timed out waiting for a backend to block in ClientRead";

# DROP DATABASE waits for every process to absorb a barrier.
my $timed_out = 0;
my $ret = $primary->psql(
	'postgres', 'DROP DATABASE dropme',
	timeout => $PostgreSQL::Test::Utils::timeout_default,
	timed_out => \$timed_out);
is($ret, 0, 'DROP DATABASE completes with a backend blocked on a read');

$reader->kill_kill;

done_testing();
