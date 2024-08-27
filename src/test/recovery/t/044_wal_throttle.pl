
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Test wait_for_replication_threshold
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

# This test depends on Perl's `kill`, which apparently is not
# portable to Windows.  (It would be nice to use Test::More's `subtest`,
# but that's not in the ancient version we require.)
if ($PostgreSQL::Test::Utils::windows_os)
{
    done_testing();
    exit;
}

# Initialize primary node, setting the replication threshold to 1KB
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf(
    'postgresql.conf', qq(
synchronous_commit = on
synchronous_standby_names = '*'
wait_for_replication_threshold = 1
));
$node_primary->start;

# Take backup
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

# Create streaming standby from backup
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
    has_streaming => 1);
$node_standby->start;

# Wait for standby to catchup
$node_primary->wait_for_catchup($node_standby);

$node_primary->safe_psql('postgres',
    "CREATE TABLE throttle_test(i int)"
);

# Pause the walreceiver process and insert data from a background psql session.
# This will create a WAL lag in the standby, leading to a throttled
# backend 'insert' session, that is waiting for the standby to catchup.
my $receiverpid = $node_standby->safe_psql('postgres',
    "SELECT pid FROM pg_stat_activity WHERE backend_type = 'walreceiver'");
like($receiverpid, qr/^[0-9]+$/, "have walreceiver pid $receiverpid");

kill 'STOP', $receiverpid;

my $throttle_h = $node_primary->background_psql('postgres');

$throttle_h->query_until(
    qr/start/, q(
\echo start
BEGIN;
INSERT INTO throttle_test SELECT 1 FROM generate_series(1, 1000000);
));

# Check for SyncRep wait event
$node_primary->poll_query_until('postgres',
    "SELECT EXISTS(SELECT * FROM pg_stat_activity WHERE wait_event = 'SyncRep')")
    or die "timed out trying to find throttled backend";

# Resume the walreceiver process and query
# until the wait event is gone
kill 'CONT', $receiverpid;

$node_primary->poll_query_until('postgres',
    "SELECT NOT EXISTS(SELECT * FROM pg_stat_activity WHERE wait_event = 'SyncRep')")
    or die "timed out waiting for throttled backend to get unblocked";

$throttle_h->quit;

done_testing();
