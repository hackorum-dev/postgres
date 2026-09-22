# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'injection points not enabled'
	unless $ENV{enable_injection_points} eq 'yes';

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq[
autovacuum = off
log_min_messages = debug1
]);
$primary->start;
$primary->safe_psql('postgres', q[
create extension injection_points;
select pg_create_physical_replication_slot('phys_slot');
]);
$primary->backup('backup');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
my $connstr = $primary->connstr;
$standby->append_conf('postgresql.conf', qq[
primary_slot_name = 'phys_slot'
primary_conninfo = '$connstr dbname=postgres'
hot_standby_feedback = on
]);
$standby->start;

$primary->safe_psql('postgres', q[
select pg_create_logical_replication_slot(
    'sync_slot', 'test_decoding', false, false, true)
]);
my $old_restart_lsn = $primary->safe_psql('postgres',
	q[select restart_lsn from pg_replication_slots where slot_name = 'sync_slot']);
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', q[show effective_wal_level]),
	'logical', 'standby replayed activation');

my $sync = $standby->background_psql('postgres');
$sync->query_until(qr/sync_started/, q(\echo sync_started
select injection_points_set_local();
select injection_points_attach('replication-slot-create-begin', 'wait');
select pg_sync_replication_slots();
));
$standby->wait_for_event('client backend',
	'replication-slot-create-begin');
pass('slot sync fetched the first incarnation');

$primary->safe_psql('postgres',
	q[select pg_drop_replication_slot('sync_slot')]);
$primary->poll_query_until('postgres',
	q[select current_setting('effective_wal_level') = 'replica'])
	or die 'timed out waiting for deactivation';
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', q[show effective_wal_level]),
	'replica', 'standby replayed deactivation');

$primary->safe_psql('postgres', q[
select pg_create_logical_replication_slot(
    'sync_slot', 'test_decoding', false, false, true)
]);
my $new_restart_lsn = $primary->safe_psql('postgres',
	q[select restart_lsn from pg_replication_slots where slot_name = 'sync_slot']);
isnt($new_restart_lsn, $old_restart_lsn,
	'second incarnation has a different restart LSN');
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', q[show effective_wal_level]),
	'logical', 'standby replayed reactivation');

$standby->safe_psql('postgres', q[
select injection_points_detach('replication-slot-create-begin');
select injection_points_wakeup('replication-slot-create-begin')
]);
$sync->quit;

is($standby->safe_psql('postgres',
	qq[select synced, temporary, invalidation_reason is null,
              restart_lsn >= '$new_restart_lsn'::pg_lsn
          from pg_replication_slots where slot_name = 'sync_slot']),
	't|f|t|t', 'slot was refetched before it was persisted');

$standby->promote;
my ($ret, $stdout, $stderr) = $standby->psql('postgres',
	q[select * from pg_logical_slot_peek_changes('sync_slot', null, null)]);
is($ret, 0, 'decoding the refetched slot succeeds');
unlike($stderr, qr/unexpected logical decoding status change/,
	'decoding does not reach a status change record');

$standby->stop;
$primary->stop;

done_testing();
