
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

# With a logical slot on the standby, promotion keeps logical decoding enabled
# and waits for every process to absorb a barrier.  Without one it doesn't
# wait at all.
$primary->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('primary_slot', 'test_decoding')");
$primary->wait_for_replay_catchup($standby);
$standby->create_logical_slot_on_standby($primary, 'standby_slot', 'postgres');

# A client that stops reading.  psql writes the COPY data to a pipe that we
# never read and blocks, so the backend fills its socket buffer and blocks in
# ClientWrite.  (Not a FETCH_COUNT query: before v17 psql runs that through a
# cursor, and the backend goes idle between fetches.)
my ($in, $out, $err) = ('', '', '');
my $writer = IPC::Run::start(
	[
		'psql', '--no-psqlrc',
		'--dbname' => $standby->connstr('postgres'),
		'--command' =>
		  "COPY (SELECT repeat('x', 1000) FROM generate_series(1, 1000000)) TO STDOUT"
	],
	'<' => \$in,
	'>' => \$out,
	'2>' => \$err);

$standby->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'ClientWrite'"
) or die "timed out waiting for a backend to block in ClientWrite";

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
# Check timed_out too: before v17, psql() returns 0 when it times out.
ok(!$timed_out && $ret == 0,
	'DROP DATABASE completes with a backend blocked on a read');

$reader->kill_kill;

done_testing();
