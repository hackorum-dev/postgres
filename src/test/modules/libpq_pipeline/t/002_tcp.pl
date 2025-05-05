
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

# Force test nodes to begin in TCP mode.
# Use an INIT block so it runs after the BEGIN block in Utils.pm.

INIT { $PostgreSQL::Test::Utils::use_unix_sockets = 0; }

use PostgreSQL::Test::Cluster;

if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\btcp\b/)
{
	plan skip_all =>
	  'Potentially unsafe test TCP not enabled in PG_TEST_EXTRA';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# Don't let PGHOST interfere with these tests.
delete $ENV{PGHOST};

my @cases = (
	$node->connstr('postgres') . " max_protocol_version=latest",
	$node->connstr('postgres') . " max_protocol_version=3.0",
	"hostaddr=127.0.0.1 port=" . $node->port);

foreach my $c (@cases)
{
	# Don't use $node->command_ok(); it overrides PGHOST too.
	command_ok(
		[ 'libpq_pipeline', 'cancel', $c ],
		"libpq_pipeline cancel, connstr: " . $c);
}

$node->stop('fast');

done_testing();
