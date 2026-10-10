# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Tests for the wal_insert_locks GUC: a valid non-default power-of-two value
# works under WAL-generating activity, and an in-range non-power-of-two value
# is rejected at server start by the check hook.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# A valid, non-default, power-of-two value is accepted and exercised.
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', 'wal_insert_locks = 16');
$node->start;

is($node->safe_psql('postgres', 'SHOW wal_insert_locks'),
	'16', 'non-default power-of-two wal_insert_locks is accepted');

# Generate WAL to exercise the reservation/insertion path with the
# non-default lock count.
$node->safe_psql('postgres',
	'CREATE TABLE t (id int); INSERT INTO t SELECT generate_series(1, 100000);'
);
is($node->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'100000', 'WAL-generating activity works with a non-default lock count');
$node->stop;

# An in-range but non-power-of-two value must be rejected at startup.
my $bad = PostgreSQL::Test::Cluster->new('bad');
$bad->init;
$bad->append_conf('postgresql.conf', 'wal_insert_locks = 12');
my $started = $bad->start(fail_ok => 1);
ok(!$started,
	'server refuses to start with a non-power-of-two wal_insert_locks');
like(
	slurp_file($bad->logfile),
	qr/must be a power of two/,
	'startup failure reports the power-of-two requirement');

# Exercise both ends of the allowed range, including operations that hold
# all insertion locks and recovery with a non-default lock count.
my $expected_rows = 100000;
for my $locks (4, 128)
{
	$node->append_conf('postgresql.conf', "wal_insert_locks = $locks");
	$node->start;
	is($node->safe_psql('postgres', 'SHOW wal_insert_locks'),
		"$locks", "wal_insert_locks = $locks is accepted");
	$node->safe_psql('postgres', 'CHECKPOINT');
	$node->pgbench(
		'--no-vacuum --client=16 --transactions=20',
		0,
		[],
		[],
		"concurrent inserts with wal_insert_locks = $locks",
		{ "locks_$locks.sql" => 'INSERT INTO t VALUES (1)' });
	$expected_rows += 320;
	$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
	$node->stop('immediate');
	$node->start;
	is($node->safe_psql('postgres', 'SELECT count(*) FROM t'),
		"$expected_rows",
		"rows survive restart with wal_insert_locks = $locks");
	$node->stop;
}

# Values outside the GUC range must also be rejected.
for my $locks (2, 256)
{
	$node->command_fails_like(
		[
			'postgres', '-D',
			$node->data_dir, '-C',
			'wal_insert_locks', '-c',
			"wal_insert_locks=$locks"
		],
		qr/outside the valid range/,
		"wal_insert_locks = $locks is out of range");
}

done_testing();
