# Copyright (c) 2026, PostgreSQL Global Development Group

# Exercise WAL reservation with concurrent inserters, segment switches and
# checkpoints.  Check recovery of records written after a checkpoint and WAL
# insertion after restart, then decode the WAL from both runs.

use strict;
use warnings FATAL => 'all';
use IPC::Run;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(extra => ['--wal-segsize=1']);
$node->append_conf(
	'postgresql.conf', qq{
autovacuum = off
checkpoint_timeout = '30min'
wal_buffers = '64kB'
wal_keep_size = '64MB'
});
$node->start;
$node->safe_psql('postgres',
	'CREATE TABLE records (writer int, seq int, PRIMARY KEY (writer, seq))');
$node->safe_psql('postgres',
	'CREATE TABLE recovery_records (id int PRIMARY KEY)');

# An empty segment needs no further switch record.
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
my $start_lsn =
  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
is( $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()'),
	$start_lsn,
	'switching an empty segment does not advance the insert position');

my $writer_script = $node->basedir . '/writer.sql';
append_to_file(
	$writer_script, q{
\set seq :seq + 1
BEGIN;
SELECT pg_advisory_xact_lock_shared(12345);
INSERT INTO records VALUES (:client_id, :seq);
SELECT pg_logical_emit_message(true, 'reservation',
    repeat('x', CASE :client_id % 3 WHEN 0 THEN 16 WHEN 1 THEN 256 ELSE 8192 END));
COMMIT;
});
my $switch_script = $node->basedir . '/switch.sql';
append_to_file(
	$switch_script, q{
SELECT pg_advisory_lock_shared(12345);
SELECT pg_switch_wal();
CHECKPOINT;
SELECT pg_advisory_unlock_shared(12345);
});

# Hold all clients at a start barrier so both kinds of work are ready before
# releasing them.  Timeouts cover both the barrier and the concurrent commands.
my $gate = $node->background_psql('postgres');
$gate->query_safe('SELECT pg_advisory_lock(12345)');
my ($writer_out, $writer_err, $switch_out, $switch_err) = ('', '', '', '');
my $writers = IPC::Run::start(
	[
		'pgbench', '-n', '-c', '8', '-j', '2', '-t', '100', '-D', 'seq=0',
		'-f', $writer_script, '-d', $node->connstr('postgres')
	],
	'>' => \$writer_out,
	'2>' => \$writer_err,
	IPC::Run::timeout($PostgreSQL::Test::Utils::timeout_default));
my $switches = IPC::Run::start(
	[
		'pgbench', '-n', '-c', '1', '-t', '8', '-f', $switch_script,
		'-d', $node->connstr('postgres')
	],
	'>' => \$switch_out,
	'2>' => \$switch_err,
	IPC::Run::timeout($PostgreSQL::Test::Utils::timeout_default));
$node->poll_query_until('postgres',
	q{SELECT count(*) = 9 FROM pg_locks WHERE locktype = 'advisory' AND NOT granted}
) or die 'WAL inserters did not reach the start barrier';
$gate->query_safe('SELECT pg_advisory_unlock(12345)');
$gate->quit;
ok($writers->finish, 'concurrent WAL writers complete') or diag($writer_err);
ok($switches->finish, 'concurrent switches and checkpoints complete')
  or diag($switch_err);

# The switcher may have checkpointed all of the concurrent inserts.  Write
# another row after a completed checkpoint so recovery must read its WAL.
$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres', 'INSERT INTO recovery_records VALUES (1)');
my $recovery_lsn =
  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$node->stop('immediate');
$node->start;
is( $node->safe_psql(
		'postgres',
		"SELECT pg_last_wal_replay_lsn() >= '$recovery_lsn'::pg_lsn"),
	't',
	'recovery replays through the post-checkpoint commit');
is($node->safe_psql('postgres', 'SELECT id FROM recovery_records'),
	'1', 'the post-checkpoint row survives crash recovery');
is($node->safe_psql('postgres', 'SELECT count(*) FROM records'),
	'800', 'all committed rows survive crash recovery');
is( $node->safe_psql(
		'postgres',
		'SELECT count(*) FROM (SELECT writer FROM records GROUP BY writer HAVING count(*) = 100) s'
	),
	'8',
	'every writer has all its committed rows after recovery');

# Exercise the reservation positions initialized by recovery.  A second
# restart and the WAL scan below also check records written after restart.
$node->safe_psql('postgres', 'INSERT INTO recovery_records VALUES (2)');
my $end_lsn =
  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$node->stop('immediate');
$node->start;
is( $node->safe_psql(
		'postgres', "SELECT pg_last_wal_replay_lsn() >= '$end_lsn'::pg_lsn"),
	't',
	'recovery replays through the commit written after restart');
is( $node->safe_psql(
		'postgres',
		q{SELECT string_agg(id::text, ',' ORDER BY id) FROM recovery_records}
	),
	'1,2',
	'rows written before and after restart survive crash recovery');

# Decoding validates record CRCs and predecessor links across page and segment
# boundaries, including the switch records and records written after restart.
# Keep the inspected WAL range on disk through the checkpoints and restarts.
$node->command_checks_all(
	[
		'pg_waldump', '--quiet',
		'--path', $node->data_dir . '/pg_wal',
		'--start', $start_lsn,
		'--end', $end_lsn
	],
	0,
	[],
	[],
	'WAL range can be decoded after crash recovery');
$node->stop;

done_testing();
