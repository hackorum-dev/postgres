# Copyright (c) 2026, PostgreSQL Global Development Group

# After primary shutdown, walreceiver must not FATAL with
# "could not send end-of-streaming message".

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use Test::More;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->start;
$node_primary->backup('backup');

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, 'backup', has_streaming => 1);
$node_standby->start;

$node_primary->wait_for_catchup($node_standby);

my $log_offset = -s $node_standby->logfile;
$node_primary->stop;

ok(!$node_standby->log_contains(
	'could not send end-of-streaming message', $log_offset),
  'walreceiver survives primary shutdown');

$node_standby->stop;

done_testing();
