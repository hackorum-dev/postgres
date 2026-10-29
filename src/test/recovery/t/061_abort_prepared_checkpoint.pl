# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that a checkpoint starting while ROLLBACK PREPARED has written its
# abort record, but has not yet updated pg_xact, waits for the abort to
# finish.  Otherwise, the checkpoint could set its redo pointer after the
# abort record and still write out the two-phase state file without the
# abort in pg_xact.  After a crash, the abort record would not be replayed
# and the transaction would come back as prepared.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 5');
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql(
	'postgres', q(
	CREATE EXTENSION injection_points;
	CREATE TABLE abort_prep (id int);
	BEGIN;
	INSERT INTO abort_prep VALUES (1);
	PREPARE TRANSACTION 'abort_prep_gxact';
));

# Stop ROLLBACK PREPARED after its abort record has been flushed, before
# pg_xact is updated.
my $rollback = $node->background_psql('postgres', on_error_stop => 0);
$rollback->query_safe(
	q(SELECT injection_points_set_local();
	  SELECT injection_points_attach('abort-prepared-before-clog', 'wait');));
$rollback->query_until(
	qr/starting_rollback/, q(
	\echo starting_rollback
	ROLLBACK PREPARED 'abort_prep_gxact';
));
$node->wait_for_event('client backend', 'abort-prepared-before-clog');

my $redo_before =
  $node->safe_psql('postgres', 'SELECT redo_lsn FROM pg_control_checkpoint()');

my $checkpoint = $node->background_psql('postgres', on_error_stop => 0);
$checkpoint->query_until(
	qr/starting_checkpoint/, q(
	\echo starting_checkpoint
	CHECKPOINT;
));

# Wait until the checkpoint either waits for the rollback, or completes.
$node->poll_query_until(
	'postgres', qq(
	SELECT EXISTS (SELECT 1 FROM pg_stat_activity
				   WHERE backend_type = 'checkpointer' AND
						 wait_event = 'CheckpointDelayStart') OR
		   (SELECT redo_lsn FROM pg_control_checkpoint()) > '$redo_before'
))
  or die "timed out waiting for the checkpoint";

is( $node->safe_psql(
		'postgres',
		'SELECT redo_lsn FROM pg_control_checkpoint()'),
	$redo_before,
	'checkpoint does not complete while ROLLBACK PREPARED is in progress');

# Crash while the rollback is still waiting.  Its abort record is flushed,
# so the transaction must not be prepared after recovery.
$node->stop('immediate');
$rollback->{run}->finish;
$checkpoint->{run}->finish;
$node->start;

is( $node->safe_psql('postgres', 'SELECT count(*) FROM pg_prepared_xacts'),
	'0', 'rolled back transaction is not prepared after crash recovery');
is($node->safe_psql('postgres', 'SELECT count(*) FROM abort_prep'),
	'0', 'row of rolled back transaction is not visible');

$node->stop;
done_testing();
