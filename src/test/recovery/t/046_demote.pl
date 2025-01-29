
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Test demote/promote actions in various scenarios using three
# nodes alpha, beta and gamma. We check proper actions results,
# correct data replication and cascade across multiple
# demote/promote, manual switchover, smart and fast demote.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Time::HiRes qw(usleep);
use Test::More;

$ENV{PGDATABASE} = 'postgres';

# Initialize node alpha
my $node_alpha = PostgreSQL::Test::Cluster->new('alpha');
$node_alpha->init(allows_streaming => 1);
$node_alpha->append_conf(
	'postgresql.conf', q[
	max_prepared_transactions = 10
	log_min_messages = debug1
]);

# Take backup
my $backup_name = 'alpha_backup';
$node_alpha->start;
$node_alpha->backup($backup_name);

# Create node beta from backup
my $node_beta = PostgreSQL::Test::Cluster->new('beta');
$node_beta->init_from_backup($node_alpha, $backup_name);
$node_beta->enable_streaming($node_alpha);
$node_beta->start;

# Create node gamma from backup
my $node_gamma = PostgreSQL::Test::Cluster->new('gamma');
$node_gamma->init_from_backup($node_alpha, $backup_name);
$node_gamma->enable_streaming($node_alpha);
$node_gamma->start;

# Create some 2PC on alpha for future tests
$node_alpha->safe_psql('postgres', q{
CREATE TABLE ins AS SELECT 1 AS i;
BEGIN;
CREATE TABLE new AS SELECT generate_series(1,5) AS i;
PREPARE TRANSACTION 'pxact1';
BEGIN;
INSERT INTO ins VALUES (2);
PREPARE TRANSACTION 'pxact2';
});

# Demote alpha.
$node_alpha->demote;

is( $node_alpha->safe_psql( 'postgres', 'SELECT pg_is_in_recovery()'),
	't', 'Node "alpha" demoted to standby' );

is( $node_alpha->safe_psql( 'postgres', "SELECT i FROM ins"),
	'1', 'Can read from table "ins" after a demote' );

# Promote alpha back in production.
$node_alpha->promote;

is( $node_alpha->safe_psql( 'postgres', 'SELECT pg_is_in_recovery()'),
	'f', 'Node "alpha" promoted after being demoted' );

# Check all 2PC xact have been restored
is( $node_alpha->safe_psql(
		'postgres',
		"SELECT string_agg(gid, ',' order by gid asc) FROM pg_prepared_xacts"),
	'pxact1,pxact2',
	"Prepared transactions still exists after demote -> promote sequence" );

# Check writing in table "ins"
is ( $node_alpha->safe_psql( 'postgres',
							 "insert into ins values (0) returning i"),
	'0', 'Can write in table "ins" after demote -> promote sequence' );

# OK
is ( $node_alpha->safe_psql( 'postgres',
	"SELECT array_agg(i::text ORDER BY i ASC) FROM ins"), '{0,1}',
	'Can read data from "ins" after demote -> promote sequence' );

# Commit one 2PC and check it on alpha and beta
is ( $node_alpha->psql( 'postgres', "commit prepared 'pxact1'"), 0,
	'Prepared transaction "pxact1" commited after a demote -> promote sequence' );

is( $node_alpha->safe_psql(
		'postgres', "SELECT 1 FROM pg_class WHERE relname = 'new'"),
	'1', 'Table "new" created from commited prepared transaction "pxact1"' );

# Check writing in table "new"
is ( $node_alpha->safe_psql( 'postgres',
	 "insert into new values (6) returning i"),
	 '6', 'Can write in table "new"' );

is( $node_alpha->safe_psql(
		'postgres', "SELECT array_agg(i::text ORDER BY i ASC) FROM new"),
	'{1,2,3,4,5,6}', "Can read data from table 'new'" );

$node_alpha->wait_for_catchup($node_beta);
$node_alpha->wait_for_catchup($node_gamma);

is( $node_beta->safe_psql(
		'postgres', "SELECT array_agg(i::text ORDER BY i ASC) FROM new"),
	'{1,2,3,4,5,6}', 'Prepared transaction "pxact1" replicated to "beta"' );

is( $node_gamma->safe_psql(
		'postgres', "SELECT array_agg(i::text ORDER BY i ASC) FROM new"),
	'{1,2,3,4,5,6}', 'prepared transaction "pxact1" replicated to "gamma"' );

# swap roles between alpha and beta

# demote alpha and check it
$node_alpha->demote;
is( $node_alpha->safe_psql( 'postgres', 'SELECT pg_is_in_recovery()'),
	't', "node alpha demoted again" );

# promote beta and check it
$node_beta->promote;
is( $node_beta->safe_psql( 'postgres', 'SELECT pg_is_in_recovery()'),
	'f', "node beta promoted" );

# Setup alpha to replicate from beta
$node_alpha->enable_streaming($node_beta);
$node_alpha->reload;

# check alpha is replicating from it
$node_beta->wait_for_catchup($node_alpha);

is( $node_beta->safe_psql(
		'postgres', 'SELECT application_name FROM pg_stat_replication'),
	$node_alpha->name, 'alpha is replicating from beta' );

# check gamma is still replicating from from alpha
$node_alpha->wait_for_catchup($node_gamma, 'write', $node_alpha->lsn('receive'));

is( $node_alpha->safe_psql(
		'postgres', 'SELECT application_name FROM pg_stat_replication'),
	$node_gamma->name, 'gamma is replicating from beta' );

# make sure the second 2PC is still available on beta
is( $node_beta->safe_psql(
		'postgres', 'SELECT gid FROM pg_prepared_xacts'),
	'pxact2', 'Second repared transactions still exists on "beta"' );

# commit the second 2PC and check its result on alpha and beta nodes
$node_beta->safe_psql( 'postgres', "commit prepared 'pxact2'");

is( $node_beta->safe_psql( 'postgres', 'SELECT 1 FROM ins WHERE i=2'),
	'1', 'prepared transaction "pxact2" commited on node "beta"' );

$node_beta->wait_for_catchup($node_alpha);
is( $node_alpha->safe_psql( 'postgres', 'SELECT 1 FROM ins WHERE i=2'),
	'1', 'prepared transaction "pxact2" streamed to node "alpha"' );

# check the 2PC has been cascaded to gamma
$node_alpha->wait_for_catchup($node_gamma, 'write', $node_alpha->lsn('receive'));
is( $node_gamma->safe_psql( 'postgres', 'SELECT 1 FROM ins WHERE i=2'),
	'1', 'Prepared transaction "pxact2" streamed to "gamma"' );

done_testing();
