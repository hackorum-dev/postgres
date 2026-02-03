# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Test that logical replication correctly rejects a physical replication
# slot if a race occurs during slot acquisition.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;
use Config;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', qq(
wal_level = logical
max_replication_slots = 2
max_wal_senders = 2
));
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

$node->safe_psql('postgres',
	q{SELECT pg_create_logical_replication_slot('race_slot', 'pgoutput');});

$node->safe_psql('postgres',
	q{SELECT injection_points_attach('start-logical-replication-before-acquire', 'wait');});

my $stderr = '';
my $pg_recvlogical = IPC::Run::start(
	[
		'pg_recvlogical',
		'--dbname' => $node->connstr('postgres'),
		'--slot' => 'race_slot',
		'--plugin' => 'pgoutput',
		'--file' => '-',
		'--start',
		'--no-loop',
	],
	'2>' => \$stderr);

# Wait for the walsender to reach the injection point.
$node->wait_for_event('walsender',
	'start-logical-replication-before-acquire');

# Replace logical slot with a physical slot of the same name.
$node->safe_psql('postgres', q{SELECT pg_drop_replication_slot('race_slot');});
$node->safe_psql('postgres',
	q{SELECT pg_create_physical_replication_slot('race_slot');});

# Resume the walsender.
$node->safe_psql('postgres',
	q{SELECT injection_points_wakeup('start-logical-replication-before-acquire');});

$pg_recvlogical->finish;
my $return = $?;

cmp_ok($return, '!=', 0, 'pg_recvlogical exited non-zero');
like($stderr,
	qr/cannot use a physical replication slot for logical decoding/,
	'logical replication refused physical slot');

$node->safe_psql('postgres', q{SELECT pg_drop_replication_slot('race_slot');});

done_testing();
