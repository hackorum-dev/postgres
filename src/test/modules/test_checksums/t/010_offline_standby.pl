# Copyright (c) 2026, PostgreSQL Global Development Group

# Test interactions between offline checksum changes and streaming replication.
# An offline change must remain local to its data directory: a standby must not
# adopt the primary's offline change, and its own offline change must survive
# WAL replay and restarts.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1, no_data_checksums => 1);
$primary->start;

$primary->backup('backup');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->start;
$primary->wait_for_replay_catchup($standby);

test_checksum_state($primary, 'off');
test_checksum_state($standby, 'off');

# An offline change on the standby must override online transitions it has
# already replayed.
enable_data_checksums($primary, wait => 'on');
wait_for_checksum_state($standby, 'on');
$primary->wait_for_replay_catchup($standby);

$standby->stop;
$standby->checksum_disable_offline;
$standby->start;
test_checksum_state($standby, 'off');

# Return both nodes to the initial state for the next test.
disable_data_checksums($primary, wait => 1);
$primary->wait_for_replay_catchup($standby);
wait_for_checksum_state($standby, 'off');

$standby->stop;
$primary->stop;

# This changes only the primary's pages and control file.
$primary->checksum_enable_offline;
$primary->start;
test_checksum_state($primary, 'on');

# Publish the primary's local state in a checkpoint, then make that checkpoint
# the starting point for crash recovery on the same node.
$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->stop('immediate');
$primary->start;
test_checksum_state($primary, 'on');

# The standby must not adopt the primary's offline transition while replaying
# the next checkpoint.
$primary->safe_psql('postgres', 'CHECKPOINT');
$standby->start;
$primary->wait_for_replay_catchup($standby);
test_checksum_state($standby, 'off');
is($standby->safe_psql('postgres', 'SELECT count(*) FROM pg_class') > 0,
	1, 'standby remains readable');

# A clean stop creates a restartpoint. Its localized checksum state must
# survive both a normal restart and subsequent crash recovery.
$standby->stop;
$standby->start;
test_checksum_state($standby, 'off');

$standby->stop('immediate');
$standby->start;
test_checksum_state($standby, 'off');

$standby->stop;
$primary->stop;

done_testing();
