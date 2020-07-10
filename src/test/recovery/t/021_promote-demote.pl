# Test demote/promote actions in various scenarios using three
# nodes alpha, beta and gamma. We check proper actions results,
# correct data replication and cascade across multiple
# demote/promote, manual switchover, smart and fast demote.

use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 24;

$ENV{PGDATABASE} = 'postgres';

# Initialize node alpha
my $node_alpha = get_new_node('alpha');
$node_alpha->init(allows_streaming => 1);
$node_alpha->append_conf(
	'postgresql.conf', qq(
	max_prepared_transactions = 10
));

# Take backup
my $backup_name = 'alpha_backup';
$node_alpha->start;
$node_alpha->backup($backup_name);

# Create node beta from backup
my $node_beta = get_new_node('beta');
$node_beta->init_from_backup($node_alpha, $backup_name);
$node_beta->enable_streaming($node_alpha);
$node_beta->start;

# Create node gamma from backup
my $node_gamma = get_new_node('gamma');
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

# create an in idle in xact session
my ($sess1_in, $sess1_out, $sess1_err) = ('', '', '');
my $sess1 = IPC::Run::start(
	[
		'psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-f', '-', '-d',
		$node_alpha->connstr('postgres')
	],
	'<', \$sess1_in,
	'>', \$sess1_out,
	'2>', \$sess1_err);

$sess1_in = q{
BEGIN;
CREATE TABLE public.test_aborted (i int);
SELECT pg_backend_pid();
};
$sess1->pump until $sess1_out =~ qr/[[:digit:]]+[\r\n]$/m;
my $sess1_pid = $sess1_out;
chomp $sess1_pid;

# create an in idle session
my ($sess2_in, $sess2_out, $sess2_err) = ('', '', '');
my $sess2 = IPC::Run::start(
	[
		'psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-f', '-', '-d',
		$node_alpha->connstr('postgres')
	],
	'<', \$sess2_in,
	'>', \$sess2_out,
	'2>', \$sess2_err);
$sess2_in = q{
SELECT pg_backend_pid();
};
$sess2->pump until $sess2_out =~ qr/\d+\s*$/m;
my $sess2_pid = $sess2_out;
chomp $sess2_pid;

$sess2_in = q{
SELECT pg_is_in_recovery();
};
$sess2->pump until $sess2_out =~ qr/(t|f)\s*$/m;

# idle session is not in recovery
is( $1, 'f', 'idle session is not in recovery' );

# Fast demote alpha.
# Secondaries beta and gamma should keep streaming from it as cascaded standbys.
# Idle in xact session should be terminate, idle session should stay alive.
$node_alpha->demote('fast');

is( $node_alpha->safe_psql( 'postgres', 'SELECT pg_is_in_recovery()'),
	't', 'node alpha demoted to standby' );

is( $node_alpha->safe_psql(
		'postgres',
		'SELECT array_agg(application_name ORDER BY application_name ASC) FROM pg_stat_replication'),
	'{beta,gamma}', 'standbys keep replicating with alpha after demote' );

# the idle in xact session should not survive the demote
is( $node_alpha->safe_psql(
		'postgres',
		qq{SELECT count(*)
		   FROM pg_catalog.pg_stat_activity
		   WHERE pid = $sess1_pid}),
	'0', 'previous idle in transaction session should be terminated' );

# table "test_aborted" has been rollbacked
is( $node_alpha->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_catalog.pg_class
		  WHERE relname='test_aborted'
		    AND relnamespace = (SELECT oid FROM pg_namespace
		                        WHERE nspname='public')}),
	'0', 'the tansaction bas been aborted during fast demote' );

# the idle session should survive the demote
is( $node_alpha->safe_psql(
		'postgres',
		qq{SELECT count(*)
		   FROM pg_catalog.pg_stat_activity
		   WHERE pid = $sess2_pid}),
	'1', "the idle session should survive the demote: $sess2_pid" );

# the idle session should report in recovery
$sess2_out = '';
$sess2_in = q{
SELECT pg_is_in_recovery();
};
$sess2->pump until $sess2_out =~ qr/(t|f)\s*$/m;

# idle session is not in recovery
is( $1, 't', 'the idle session reports in recovery' );

# close both sessions
$sess1_out = $sess2_out = $sess1_in = $sess2_in = '';
$sess1->finish;
$sess2->finish;

# Promote alpha back in production.
$node_alpha->promote;

is( $node_alpha->safe_psql( 'postgres', 'SELECT pg_is_in_recovery()'),
	'f', "node alpha promoted" );

# Check all 2PC xact have been restored
is( $node_alpha->safe_psql(
		'postgres',
		"SELECT string_agg(gid, ',' order by gid asc) FROM pg_prepared_xacts"),
	'pxact1,pxact2', "prepared transactions 'pxact1' and 'pxact2' exists" );

# Commit one 2PC and check it on alpha and beta
$node_alpha->safe_psql( 'postgres', "commit prepared 'pxact1'");

is( $node_alpha->safe_psql(
		'postgres', "SELECT array_agg(i::text ORDER BY i ASC) FROM new"),
	'{1,2,3,4,5}', "prepared transaction 'pxact1' commited" );

$node_alpha->wait_for_catchup($node_beta);
$node_alpha->wait_for_catchup($node_gamma);

is( $node_beta->safe_psql(
		'postgres', "SELECT array_agg(i::text ORDER BY i ASC) FROM new"),
	'{1,2,3,4,5}', "prepared transaction 'pxact1' replicated to beta" );

is( $node_gamma->safe_psql(
		'postgres', "SELECT array_agg(i::text ORDER BY i ASC) FROM new"),
	'{1,2,3,4,5}', "prepared transaction 'pxact1' replicated to gamma" );

# create another idle in xact session
$sess1_in = q{
BEGIN;
CREATE TABLE public.test_succeed (i int);
SELECT pg_backend_pid();
};
$sess1->pump until $sess1_out =~ qr/\d+\s*$/m;
$sess1_pid = $sess1_out;
chomp $sess1_pid;

# swap roles between alpha and beta

# Demote alpha in smart mode.
# Don't wait for demote to complete here so we can use sess1
# to keep doing some more write activity before commit and demote.
is( $node_alpha->safe_psql( 'postgres', 'SELECT pg_demote(false, false)'),
	't', "demote signal sent to node alpha" );

# wait for the demote to begin and wait for active xact.
my $fh;
while (1) {
	my $status;
	open my $fh, '<', $node_alpha->data_dir . '/postmaster.pid';
	$status = $_ while <$fh>;
	close $fh;
	chomp($status);
	last if $status eq 'demoting';
	sleep 1;
}

# make sure the demote waits for running xacts
sleep 2;

# test no new session possible during demote
$sess2_in = q{
SELECT 1;
};
$sess2->start;
$sess2->finish;
ok( $sess2_err =~ /FATAL:  the database system is demoting\s$/, 'session rejected during demote process');

# add some write activity on demote-blocking session sess1
$sess1_out = '';
$sess1_in = q{
INSERT INTO public.test_succeed VALUES (1) RETURNING i;
COMMIT;
};
$sess1->pump until $sess1_out =~ qr/\d+\s*$/m;
$sess1->finish;

chomp($sess1_out);
is($sess1_out, '1', 'session in active xact able to write the smart demote signal');

$node_alpha->poll_query_until('postgres', 'SELECT pg_is_in_recovery()', 't');

is( $node_alpha->safe_psql( 'postgres', 'SELECT pg_is_in_recovery()'),
	't', "node alpha demoted" );

# fetch the last REDO location from alpha and chek beta received everyting
my ($stdout, $stderr) = run_command([ 'pg_controldata', $node_alpha->data_dir ]);
$stdout =~ m{REDO location:\s+([0-9A-F]+/[0-9A-F]+)$}mg;
my $redo_loc = $1;

is( $node_beta->safe_psql(
		'postgres',
		"SELECT pg_wal_lsn_diff(pg_last_wal_receive_lsn(), '$redo_loc') > 0 "),
	't', "node beta received the demote checkpoint from alpha" );

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
	'pxact2', "prepared transactions pxact2' exists" );

# commit the second 2PC and check its result on alpha and beta nodes
$node_beta->safe_psql( 'postgres', "commit prepared 'pxact2'");

is( $node_beta->safe_psql( 'postgres', 'SELECT 1 FROM ins WHERE i=2'),
	'1', "prepared transaction 'pxact2' commited" );

$node_beta->wait_for_catchup($node_alpha);
is( $node_alpha->safe_psql( 'postgres', 'SELECT 1 FROM ins WHERE i=2'),
	'1', "prepared transaction 'pxact2' streamed to alpha" );

# check the 2PC has been cascaded to gamma
$node_alpha->wait_for_catchup($node_gamma, 'write', $node_alpha->lsn('receive'));
is( $node_gamma->safe_psql( 'postgres', 'SELECT 1 FROM ins WHERE i=2'),
	'1', "prepared transaction 'pxact2' streamed to gamma" );
