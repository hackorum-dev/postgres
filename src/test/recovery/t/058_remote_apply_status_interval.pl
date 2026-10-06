# Copyright (c) 2026, PostgreSQL Global Development Group

# Check that a commit with synchronous_commit = remote_apply completes when
# the standby has wal_receiver_status_interval = 0.  Periodic status updates
# are disabled then, but the walreceiver must still send the apply reply
# requested by the startup process.
#
# Keepalives from the walsender and pings from the walreceiver are disabled
# too, so the apply reply is the only message that can release the waiter.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', "wal_sender_timeout = 0");
$primary->start;
$primary->safe_psql('postgres', 'CREATE TABLE t (a int)');
$primary->backup('bkp');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->append_conf(
	'postgresql.conf', qq(
wal_receiver_status_interval = 0
wal_receiver_timeout = 0
hot_standby_feedback = off
));
$standby->start;

# Make the standby synchronous only now, so that the setup above does not
# wait for it.
$primary->safe_psql('postgres',
	"ALTER SYSTEM SET synchronous_standby_names = '*'");
$primary->reload;
$primary->poll_query_until(
	'postgres',
	"SELECT count(*) = 1 FROM pg_stat_replication
	 WHERE state = 'streaming' AND sync_state = 'sync'
	   AND flush_lsn IS NOT NULL"
) or die "timed out waiting for standby to become synchronous";

# Pause replay so that the commit cannot be applied yet, and check that the
# backend really enters the SyncRep wait.  Otherwise the idle check below
# could pass before the INSERT has even started.
$standby->safe_psql('postgres', 'SELECT pg_wal_replay_pause()');

my $bg = $primary->background_psql('postgres', on_error_stop => 0);
my $pid = $bg->query_safe('SELECT pg_backend_pid()');
$bg->query_safe('SET synchronous_commit = remote_apply');
$bg->query_until(qr/start/, "\\echo start\nINSERT INTO t VALUES (1);\n");

ok( $primary->poll_query_until(
		'postgres',
		"SELECT wait_event = 'SyncRep' FROM pg_stat_activity
		 WHERE pid = $pid"),
	'remote_apply commit waits for standby replay');

$standby->safe_psql('postgres', 'SELECT pg_wal_replay_resume()');

ok( $primary->poll_query_until(
		'postgres',
		"SELECT state = 'idle' FROM pg_stat_activity WHERE pid = $pid"),
	'remote_apply commit completes with wal_receiver_status_interval = 0');
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'1', 'remote_apply commit is visible on standby');

# Unblock the session if it is still waiting.  The wait above may have used
# up the session timer, so restart it before quitting.
$primary->safe_psql('postgres', "SELECT pg_cancel_backend($pid)");
$bg->set_query_timer_restart;
$bg->query('SELECT 1');
$bg->quit;

$standby->stop;
$primary->stop;

done_testing();
