
# Copyright (c) 2025, PostgreSQL Global Development Group

# Try to create a WAL record which is larger than the limit XLogRecordMaxSize.
# That should raise an error.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use Test::More;

# Disable the test unless requested explicitly because it requires 1Gb of memory
if (exists $ENV{PG_TEST_EXTRA}
	&& $ENV{PG_TEST_EXTRA} =~ m/\boversized_wal_record\b/)
{
	plan tests => 4;
}
else
{
	plan skip_all =>
      'Skipping oversized_wal_record test as it requires a lot of memory';
}

note "Check handing of oversized WAL records";

# Initialize and start basic node
my $node = PostgreSQL::Test::Cluster->new('node');

$node->init;

$node->start;

# Insert oversized record
my ($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	"select pg_logical_emit_message(false, 'a', repeat('00000000', (1020 * 1024 * 1024)/8+1), true)",
	on_error_die => 0
  );

# Verify we have error
ok($ret != 0, 'exit code is non-zero');

ok($stderr =~ qr/ERROR:  oversized WAL record/, 'expect error');

ok($stderr =~ qr/DETAIL:  WAL record would be [0-9]+ bytes \(of maximum [0-9]+ bytes\);/,
   'error details');

$node->stop;

done_testing();
