# Copyright (c) 2026, PostgreSQL Global Development Group

#
# Hint bits set on a hot standby are not WAL-logged.  If the standby replayed
# a commit that the new primary never received, and set HEAP_XMIN_COMMITTED
# for it, pg_rewind must still copy that block from the source.
#
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_a = PostgreSQL::Test::Cluster->new('node_a');
$node_a->init(allows_streaming => 1, extra => ['-k']);
$node_a->append_conf(
	'postgresql.conf', qq(
wal_log_hints = on
max_prepared_transactions = 5
shared_buffers = '128MB'
autovacuum = off
checkpoint_timeout = '1h'
max_wal_size = '1GB'
wal_keep_size = '100MB'
));
$node_a->start;
$node_a->safe_psql('postgres', 'CREATE EXTENSION pg_buffercache');
$node_a->safe_psql('postgres', 'CREATE EXTENSION pageinspect');

$node_a->backup('b');
my $node_s1 = PostgreSQL::Test::Cluster->new('node_s1');
$node_s1->init_from_backup($node_a, 'b', has_streaming => 1);
$node_s1->start;
my $node_s2 = PostgreSQL::Test::Cluster->new('node_s2');
$node_s2->init_from_backup($node_a, 'b', has_streaming => 1);
$node_s2->start;

$node_a->safe_psql('postgres',
	'CREATE TABLE c(id int); INSERT INTO c SELECT generate_series(1,100); CHECKPOINT;'
);

my $sess = $node_a->background_psql('postgres');
$sess->query_safe('BEGIN; INSERT INTO c VALUES (999999);');
my $xid = $sess->query_safe('SELECT pg_current_xact_id()');
$sess->query_safe("PREPARE TRANSACTION 'p'");
$node_a->safe_psql('postgres', 'CHECKPOINT');
$node_a->wait_for_replay_catchup($node_s1);
$node_a->wait_for_replay_catchup($node_s2);

if ($ENV{EVICT_BEFORE_FORK})
{
	# Move S2's minRecoveryPoint past c's page LSN, but before the fork, so
	# writing c's page later does not advance it.
	$node_a->safe_psql('postgres',
		'CREATE TABLE d(id int); INSERT INTO d VALUES (1);');
	$node_a->wait_for_replay_catchup($node_s1);
	$node_a->wait_for_replay_catchup($node_s2);
	$node_s2->safe_psql('postgres',
		"SELECT pg_buffercache_evict(bufferid) FROM pg_buffercache WHERE isdirty");
}

# Detach S1 from A.
$node_s1->append_conf('postgresql.conf', "primary_conninfo = ''");
$node_s1->reload;
$node_s1->poll_query_until('postgres',
	'SELECT count(*) = 0 FROM pg_stat_wal_receiver')
  or die 'S1 did not disconnect';

$sess->quit;
$node_a->safe_psql('postgres', "ROLLBACK PREPARED 'p'");
$node_a->safe_psql('postgres', 'CHECKPOINT') if $ENV{CKPT_AFTER};
$node_a->wait_for_replay_catchup($node_s2);

# Abort hints need no flush check, so no eviction trick here.
is($node_s2->safe_psql('postgres', 'SELECT count(*) FROM c'),
	'100', 'S2 does not see the rolled back row before rewind');
note 'S2 control: ' . join(' ', grep { /Minimum recovery ending location|Latest checkpoint location/ } split /\n/, `pg_controldata $node_s2->{_basedir}/pgdata`);

my $hint_q = "SELECT (t_infomask & 512) <> 0 FROM heap_page_items(get_raw_page('c', 0))
	 WHERE t_xmin::text = '$xid'";

is($node_s2->safe_psql('postgres', $hint_q), 't',
	'HEAP_XMIN_INVALID set on S2 before rewind');
$node_s2->stop('fast');
note 'S2 control after stop: ' . join(' ', grep { /Minimum recovery ending location|Latest checkpoint location/ } split /\n/, `pg_controldata $node_s2->{_basedir}/pgdata`);
$node_s1->promote;
# Otherwise replay starts at S1's old restartpoint and the INSERT's FPW hides
# the problem.
$node_s1->safe_psql('postgres', 'CHECKPOINT');
$node_a->stop('immediate');
# S1 recovered the prepared transaction and commits it on its timeline.
$node_s1->safe_psql('postgres', "COMMIT PREPARED 'p'");

my ($out, $err) = run_command(
	[
		'pg_rewind',
		'--source-server' => $node_s1->connstr('postgres'),
		'--target-pgdata' => $node_s2->data_dir,
		'--write-recovery-conf',
	]);
note "pg_rewind: $err";
# pg_rewind copied S1's postgresql.conf
$node_s2->append_conf('postgresql.conf', 'port = ' . $node_s2->port);
$node_s2->start;
$node_s1->wait_for_replay_catchup($node_s2);

is($node_s2->safe_psql('postgres', $hint_q), 'f',
	'no HEAP_XMIN_INVALID on S2 after rewind');
is($node_s2->safe_psql('postgres', 'SELECT count(*) FROM c WHERE id = 999999'),
	'1', 'committed row visible on S2 after rewind');

# Reading c on S1 emits an FPI_FOR_HINT that would mask the problem on S2.
is($node_s1->safe_psql('postgres', 'SELECT count(*) FROM c WHERE id = 999999'),
	'1', 'row visible on S1');

done_testing();
