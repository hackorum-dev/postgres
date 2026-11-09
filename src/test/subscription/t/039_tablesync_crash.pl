# Copyright (c) 2026, PostgreSQL Global Development Group

# A table whose initial copy has completed must finish synchronizing after a
# subscriber crash that happens while its tablesync worker is dropping the
# tablesync slot on the publisher, either just before or just after the drop.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Insert, update and delete rows on the publisher.  Comparing the final table
# contents then shows that the subscriber matches the publisher.
sub mutate
{
	my ($node, $first, $last) = @_;

	$node->safe_psql(
		'postgres', qq[
INSERT INTO tab_sync SELECT i, 'new-' || i FROM generate_series($first, $last) i;
UPDATE tab_sync SET v = 'updated-$last' WHERE id = 10;
DELETE FROM tab_sync WHERE id = $first - 100;
]);
	return;
}

sub table_contents
{
	my ($node) = @_;

	return $node->safe_psql('postgres',
		"SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM tab_sync");
}

my $slots_query =
  "SELECT count(*) FROM pg_replication_slots WHERE slot_name LIKE 'pg_%_sync_%'";
my $origins_query =
  "SELECT count(*) FROM pg_replication_origin WHERE roname ~ '^pg_[0-9]+_[0-9]+\$'";

# slot_exists is whether the tablesync slot is still on the publisher when the
# worker is paused, and so whether the restarted worker has a slot to clean up.
my @cases = (
	{
		name => 'before slot drop',
		point => 'tablesync-before-source-slot-drop',
		slot_exists => 1,
	},
	{
		name => 'after slot drop',
		point => 'tablesync-after-source-slot-drop',
		slot_exists => 0,
	});

my $case_number = 0;
foreach my $case (@cases)
{
	my ($name, $point) = ($case->{name}, $case->{point});

	$case_number++;

	my $publisher = PostgreSQL::Test::Cluster->new("publisher_$case_number");
	$publisher->init(allows_streaming => 'logical');
	$publisher->start;

	# Make crash recovery depend on what was actually flushed.
	my $subscriber = PostgreSQL::Test::Cluster->new("subscriber_$case_number");
	$subscriber->init;
	$subscriber->append_conf('postgresql.conf',
		"fsync = on\nwal_retrieve_retry_interval = '100ms'");
	$subscriber->start;

	$subscriber->safe_psql('postgres', 'CREATE EXTENSION injection_points');

	for my $node ($publisher, $subscriber)
	{
		$node->safe_psql('postgres',
			'CREATE TABLE tab_sync (id int PRIMARY KEY, v text)');
	}
	$publisher->safe_psql('postgres',
		"INSERT INTO tab_sync SELECT i, 'orig-' || i FROM generate_series(1, 100) i"
	);
	$publisher->safe_psql('postgres',
		'CREATE PUBLICATION pub FOR TABLE tab_sync');

	# Pause the tablesync worker at the injection point.  The subscription
	# commits synchronously so that FINISHEDCOPY survives the crash.
	$subscriber->safe_psql('postgres',
		"SELECT injection_points_attach('$point', 'wait')");
	my $connstr = $publisher->connstr . ' dbname=postgres';
	$subscriber->safe_psql('postgres',
		"CREATE SUBSCRIPTION sub CONNECTION '$connstr' PUBLICATION pub WITH (synchronous_commit = on)"
	);
	$subscriber->wait_for_event('logical replication tablesync worker',
		$point);

	is($publisher->safe_psql('postgres', $slots_query),
		$case->{slot_exists},
		"$name: tablesync slot presence on the publisher at the pause");

	# Make changes on both sides of the crash.  SYNCDONE must already be
	# durable, so that the restarted worker can finish the cleanup.
	mutate($publisher, 101, 105);
	$subscriber->stop('immediate');
	$subscriber->start;
	mutate($publisher, 106, 110);

	$subscriber->poll_query_until('postgres',
		"SELECT srsubstate FROM pg_subscription_rel WHERE srrelid = 'tab_sync'::regclass",
		'r')
	  or die "timed out waiting for the table to become ready ($name)";
	$publisher->wait_for_catchup('sub');

	is( table_contents($subscriber),
		table_contents($publisher),
		"$name: table has the same contents as on the publisher");
	is($publisher->safe_psql('postgres', $slots_query),
		'0', "$name: no tablesync slot is left on the publisher");
	is($subscriber->safe_psql('postgres', $origins_query),
		'0', "$name: no tablesync origin is left on the subscriber");

	$subscriber->stop;
	$publisher->stop;
}

done_testing();
