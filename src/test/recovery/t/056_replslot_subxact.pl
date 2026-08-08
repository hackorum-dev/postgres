# Copyright (c) 2026, PostgreSQL Global Development Group

# A SQL slot function acquires MyReplicationSlot and releases it before
# returning.  If it errors after acquiring the slot and the error is caught by
# a PL/pgSQL EXCEPTION handler, the release is skipped and the slot leaks; the
# next slot operation in the session then trips Assert(!MyReplicationSlot).
# Check that a subtransaction abort releases such a slot.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(allows_streaming => 'logical');
$node->start;

# Acquire path: pg_replication_slot_advance() acquires the slot and then errors
# because it has never reserved WAL.  After the caught error the follow-up
# create must succeed.
my $count = $node->safe_psql('postgres', q{
DO $$
BEGIN
	PERFORM pg_create_physical_replication_slot('advance_src', false);
	BEGIN
		PERFORM pg_replication_slot_advance('advance_src', '0/1');
	EXCEPTION WHEN object_not_in_prerequisite_state THEN
		NULL;
	END;
	PERFORM pg_create_physical_replication_slot('advance_next');
END $$;
SELECT count(*) FROM pg_replication_slots
 WHERE slot_name IN ('advance_src', 'advance_next');
});
is($count, '2', 'slot released after caught pg_replication_slot_advance error');

# Create path: creating a logical slot assigns MyReplicationSlot before the
# output plugin is loaded, so a missing plugin errors with the slot acquired.
$count = $node->safe_psql('postgres', q{
DO $$
BEGIN
	BEGIN
		PERFORM pg_create_logical_replication_slot('create_bad', 'no_such_plugin');
	EXCEPTION WHEN OTHERS THEN
		NULL;
	END;
	PERFORM pg_create_physical_replication_slot('create_next');
END $$;
SELECT count(*) FROM pg_replication_slots WHERE slot_name = 'create_next';
});
is($count, '1', 'slot released after caught pg_create_logical_replication_slot error');

$node->stop;
done_testing();
