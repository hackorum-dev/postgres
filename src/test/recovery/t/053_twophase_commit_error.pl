# Copyright (c) 2026, PostgreSQL Global Development Group

# Test failure when COMMIT PREPARED gets an ERROR after its commit WAL record
# has been emitted, but before two-phase cleanup callbacks have released locks.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 5');
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql(
	'postgres', q{
	SELECT injection_points_attach('twophase-callbacks-execution', 'error')
});

$node->safe_psql(
	'postgres', q{
	CREATE TABLE twophase_lock_target(id int);
	CREATE TABLE twophase_commit_marker(id int);

	BEGIN;
	LOCK TABLE twophase_lock_target IN ACCESS EXCLUSIVE MODE;
	INSERT INTO twophase_commit_marker VALUES (1);
	PREPARE TRANSACTION 'prepared';
});

my $table_oid = $node->safe_psql(
	'postgres', q{
	SELECT oid FROM pg_class WHERE relname = 'twophase_lock_target';
});


my ($ret, $stdout, $stderr) =
  $node->psql('postgres', q{COMMIT PREPARED 'prepared'});

isnt($ret, 0, 'COMMIT PREPARED failed at injectcion point');

$ret = $node->safe_psql(
	'postgres', q{
	SELECT count(*) FROM pg_prepared_xacts;
});
is($ret, 0, 'we cannot see transaction in the catalog');

$ret = $node->safe_psql(
	'postgres', q{
	SELECT * FROM twophase_commit_marker;
});
is($ret, 1, 'transaction has been commited');


$ret = $node->safe_psql(
	'postgres', qq{
	SELECT COUNT(*) FROM pg_locks WHERE locktype = 'relation' AND relation = $table_oid;
});
is($ret, 1, 'orphaned lock detected');

$node->stop;
done_testing();
