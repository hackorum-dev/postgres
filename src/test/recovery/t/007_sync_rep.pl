
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Minimal test testing synchronous replication sync_state transition
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Query checking sync_priority and sync_state of each standby
my $check_sql =
  "SELECT application_name, sync_priority, sync_state FROM pg_stat_replication ORDER BY application_name;";

# Check that sync_state of each standby is expected (waiting till it is).
# If $setting is given, synchronous_standby_names is set to it and
# the configuration file is reloaded before the test.
sub test_sync_state
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($self, $expected, $msg, $setting) = @_;

	if (defined($setting))
	{
		$self->safe_psql('postgres',
			"ALTER SYSTEM SET synchronous_standby_names = '$setting';");
		$self->reload;
	}

	ok($self->poll_query_until('postgres', $check_sql, $expected), $msg);
	return;
}

# Start a standby and check that it is registered within the WAL sender
# array of the given primary.  This polls the primary's pg_stat_replication
# until the standby is confirmed as registered.
sub start_standby_and_wait
{
	my ($primary, $standby) = @_;
	my $primary_name = $primary->name;
	my $standby_name = $standby->name;
	my $query =
	  "SELECT count(1) = 1 FROM pg_stat_replication WHERE application_name = '$standby_name'";

	$standby->start;

	print("### Waiting for standby \"$standby_name\" on \"$primary_name\"\n");
	$primary->poll_query_until('postgres', $query);
	return;
}

sub check_replication_state
{
	my ($source_node, $node_standby_1, $node_standby_2,
		$node_standby_3, $node_standby_4, $backup_name) = @_;

	my $stdby1_name = $node_standby_1->name;
	my $stdby2_name = $node_standby_2->name;
	my $stdby3_name = $node_standby_3->name;
	my $stdby4_name = $node_standby_4->name;

	# Check that sync_state is determined correctly when
	# synchronous_standby_names is specified in old syntax.
	test_sync_state(
		$source_node,
		"$stdby1_name|1|sync
$stdby2_name|2|potential
$stdby3_name|0|async",
		'old syntax of synchronous_standby_names',
		"$stdby1_name,$stdby2_name");

	# Check that all the standbys are considered as either sync or
	# potential when * is specified in synchronous_standby_names.
	# Note that standby1 is chosen as sync standby because
	# it's stored in the head of WalSnd array which manages
	# all the standbys though they have the same priority.
	test_sync_state(
		$source_node,
		"$stdby1_name|1|sync
$stdby2_name|1|potential
$stdby3_name|1|potential",
		'asterisk in synchronous_standby_names',
		'*');

	# Stop and start standbys to rearrange the order of standbys
	# in WalSnd array. Now, if standbys have the same priority,
	# standby2 is selected preferentially and standby3 is next.
	$node_standby_1->stop;
	$node_standby_2->stop;
	$node_standby_3->stop;

	# Make sure that each standby reports back to the primary in the wanted
	# order.
	start_standby_and_wait($source_node, $node_standby_2);
	start_standby_and_wait($source_node, $node_standby_3);

	# Specify 2 as the number of sync standbys.
	# Check that two standbys are in 'sync' state.
	test_sync_state(
		$source_node,
		"$stdby2_name|2|sync
$stdby3_name|3|sync",
		'2 synchronous standbys',
		"2($stdby1_name,$stdby2_name,$stdby3_name)");

	# Start standby1
	start_standby_and_wait($source_node, $node_standby_1);

	$node_standby_4->init_from_backup($source_node, $backup_name,
		has_streaming => 1);
	$node_standby_4->start;

	# Check that standby1 and standby2 whose names appear earlier in
	# synchronous_standby_names are considered as sync. Also check that
	# standby3 appearing later represents potential, and standby4 is
	# in 'async' state because it's not in the list.
	test_sync_state(
		$source_node,
		"$stdby1_name|1|sync
$stdby2_name|2|sync
$stdby3_name|3|potential
$stdby4_name|0|async",
		'2 sync, 1 potential, and 1 async');

	# Check that sync_state of each standby is determined correctly
	# when num_sync exceeds the number of names of potential sync standbys
	# specified in synchronous_standby_names.
	test_sync_state(
		$source_node,
		"$stdby1_name|0|async
$stdby2_name|4|sync
$stdby3_name|3|sync
$stdby4_name|1|sync",
		'num_sync exceeds the num of potential sync standbys',
		"6($stdby4_name,standby,$stdby3_name,$stdby2_name)");

	# The setting that * comes before another standby name is acceptable
	# but does not make sense in most cases. Check that sync_state is
	# chosen properly even in case of that setting. standby1 is selected
	# as synchronous as it has the highest priority, and is followed by a
	# second standby listed first in the WAL sender array, which is
	# standby2 in this case.
	test_sync_state(
		$source_node,
		"$stdby1_name|1|sync
$stdby2_name|2|sync
$stdby3_name|2|potential
$stdby4_name|2|potential",
		'asterisk before another standby name',
		"2($stdby1_name,*,$stdby2_name)");

	# Check that the setting of '2(*)' chooses standby2 and standby3 that are stored
	# earlier in WalSnd array as sync standbys.
	test_sync_state(
		$source_node,
		"$stdby1_name|1|potential
$stdby2_name|1|sync
$stdby3_name|1|sync
$stdby4_name|1|potential",
		'multiple standbys having the same priority are chosen as sync',
		'2(*)');

	# Stop Standby3 which is considered in 'sync' state.
	$node_standby_3->stop;

	# Check that the state of standby1 stored earlier in WalSnd array than
	# standby4 is transited from potential to sync.
	test_sync_state(
		$source_node,
		"$stdby1_name|1|sync
$stdby2_name|1|sync
$stdby4_name|1|potential",
		'potential standby found earlier in array is promoted to sync');

	# Check that standby1 and standby2 are chosen as sync standbys
	# based on their priorities.
	test_sync_state(
		$source_node,
		"$stdby1_name|1|sync
$stdby2_name|2|sync
$stdby4_name|0|async",
		'priority-based sync replication specified by FIRST keyword',
		"FIRST 2($stdby1_name, $stdby2_name)");

	# Check that all the listed standbys are considered as candidates
	# for sync standbys in a quorum-based sync replication.
	test_sync_state(
		$source_node,
		"$stdby1_name|1|quorum
$stdby2_name|1|quorum
$stdby4_name|0|async",
		'2 quorum and 1 async',
		"ANY 2($stdby1_name, $stdby2_name)");

	# Start Standby3 which will be considered in 'quorum' state.
	$node_standby_3->start;

	# Check that the setting of 'ANY 2(*)' chooses all standbys as
	# candidates for quorum sync standbys.
	test_sync_state(
		$source_node,
		"$stdby1_name|1|quorum
$stdby2_name|1|quorum
$stdby3_name|1|quorum
$stdby4_name|1|quorum",
		'all standbys are considered as candidates for quorum sync standbys',
		'ANY 2(*)');

	return;
}

# Cluster topology (the arrow shows the direction of replication)
#
#							 		  |-> cascade_standby10 |-> standby
#							 		  |
# 							 		  |-> cascade_standby11
# 			 |-> cascade_standby00 -> |
#			 |				 		  |-> cascade_standby12
#			 |				 		  |
#			 |				 		  |-> cascade_standby13
# 			 |
#			 |				 		  |-> standby00
#			 |				 		  |
# 			 |				 		  |-> standby01
# 			 |-> cascade_standby01 -> |
#			 |				 		  |-> standby02
#			 |				 		  |
# primary -> |				 		  |-> standby03
# 			 |
#			 |				 		  |-> standby10
#			 |				 		  |
# 			 |				 		  |-> standby11
# 			 |-> cascade_standby02 -> |
#			 |				 		  |-> standby12
#			 |				 		  |
#			 |				 		  |-> standby13
#			 |
#			 |-> cascade_standby03

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->start;
my $backup_name = 'primary_backup';

# Take backup
$node_primary->backup($backup_name);

# Create all the standbys.  Their status on the primary is checked to ensure
# the ordering of each one of them in the WAL sender array of the primary.

my @cascade_standbys0;
my @cascade_standby0_names;

my $node;
my $i = 0;
for (; $i < 3; $i++)
{
	push @cascade_standby0_names, "cascade_standby0$i";

	$node = PostgreSQL::Test::Cluster->new($cascade_standby0_names[$i]);
	$node->init_from_backup($node_primary, $backup_name, has_streaming => 1);
	start_standby_and_wait($node_primary, $node);

	push @cascade_standbys0, $node;
}

# This cascade standby will be initted in check_replication_state function
push @cascade_standby0_names, "cascade_standby0$i";

$node = PostgreSQL::Test::Cluster->new($cascade_standby0_names[$i]);
push @cascade_standbys0, $node;
$i = 0;

# Create cascade standby and it`s last standbys.
$backup_name = 'cascade_standby00_backup';

# Take backup of cascade_standby00
$cascade_standbys0[0]->backup($backup_name);

my @cascade_standbys1;
my @cascade_standby1_names;
for (; $i < 3; $i++)
{
	push @cascade_standby1_names, "cascade_standby1$i";

	$node = PostgreSQL::Test::Cluster->new($cascade_standby1_names[$i]);
	$node->init_from_backup($cascade_standbys0[0], $backup_name,
		has_streaming => 1);
	start_standby_and_wait($cascade_standbys0[0], $node);

	push @cascade_standbys1, $node;
}

# This cascade standby will be initted in check_replication_state function
push @cascade_standby1_names, "cascade_standby1$i";

$node = PostgreSQL::Test::Cluster->new($cascade_standby1_names[$i]);
push @cascade_standbys1, $node;
$i = 0;

# Take backup
$backup_name = 'cascade_standby10_backup';
$cascade_standbys1[0]->backup($backup_name);

# Create standby5 linking to cascade standby
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($cascade_standbys1[0], $backup_name,
	has_streaming => 1);
start_standby_and_wait($cascade_standbys1[0], $standby);

my $standby_name = $standby->name;

# Take backup of cascade_standby01
$backup_name = 'cascade_standby01_backup';
$cascade_standbys0[1]->backup($backup_name);

my @standbys0;
for (; $i < 3; $i++)
{
	$node = PostgreSQL::Test::Cluster->new("standby0$i");
	$node->init_from_backup($cascade_standbys0[1], $backup_name,
		has_streaming => 1);
	start_standby_and_wait($cascade_standbys0[1], $node);

	push @standbys0, $node;
}

# This cascade_standby will be initted in check_replication_state function
$node = PostgreSQL::Test::Cluster->new("standby0$i");
push @standbys0, $node;
$i = 0;

# Take backup of cascade_standby02
$backup_name = 'cascade_standby02_backup';
$cascade_standbys0[2]->backup($backup_name);

my @standbys1;
for ($i = 0; $i < 3; $i++)
{
	$node = PostgreSQL::Test::Cluster->new("standby1$i");
	$node->init_from_backup($cascade_standbys0[2], $backup_name,
		has_streaming => 1);
	start_standby_and_wait($cascade_standbys0[2], $node);

	push @standbys1, $node;
}

# This cascade standby will be initted in check_replication_state function
$node = PostgreSQL::Test::Cluster->new("standby1$i");
push @standbys1, $node;

# Set up initial topology

test_sync_state(
	$node_primary,
	"${cascade_standby0_names[0]}|1|sync
${cascade_standby0_names[1]}|2|sync
${cascade_standby0_names[2]}|3|sync",
	'set up initial topology for primary server and it`s standbys',
	"FIRST 3 (${cascade_standby0_names[0]}, ${cascade_standby0_names[1]}, ${cascade_standby0_names[2]})"
);

test_sync_state(
	$cascade_standbys0[0],
	"${cascade_standby1_names[0]}|1|quorum
${cascade_standby1_names[1]}|1|quorum
${cascade_standby1_names[2]}|0|async",
	'set up initial topology for cascade standby server and it`s standbys',
	"ANY 2 (${cascade_standby1_names[0]}, ${cascade_standby1_names[1]})");

test_sync_state(
	$cascade_standbys1[0], "$standby_name|1|sync",
	'make sync standby 5', "$standby_name");

my ($stdout, $stderr, $timed_out);

# Check that queries working
my $cmdret = $node_primary->psql(
	'postgres', 'CREATE TABLE test (id integer, data text);',
	stdout => \$stdout,
	stderr => \$stderr,
	timeout => $PostgreSQL::Test::Utils::timeout_default,
	timed_out => \$timed_out,
	on_error_die => 1,
	extra_params => ['--single-transaction']);

ok($cmdret == 0 && $timed_out == 0, "Query works");

# Try to stop standby 5
$standby->stop;

# This query needs only for status of sync replicas can be changed
$cmdret = $node_primary->psql(
	'postgres', 'INSERT INTO test VALUES (1, \'first insertion\')',
	stdout => \$stdout,
	stderr => \$stderr,
	timeout => $PostgreSQL::Test::Utils::timeout_default,
	timed_out => \$timed_out,
	on_error_die => 1,
	extra_params => ['--single-transaction']);

ok($timed_out == 1, "Query fails with time out");

test_sync_state(
	$node_primary,
	"${cascade_standby0_names[0]}|1|sync?
${cascade_standby0_names[1]}|2|sync
${cascade_standby0_names[2]}|3|sync",
	"check that ${cascade_standby0_names[0]} in primary marks as invalid");

test_sync_state(
	$cascade_standbys0[0],
	"${cascade_standby1_names[0]}|1|quorum?
${cascade_standby1_names[1]}|1|quorum
${cascade_standby1_names[2]}|0|async",
	"check that ${cascade_standby1_names[0]} in cascade standby marks as invalid"
);

$standby->start;

$cmdret = $node_primary->psql(
	'postgres', 'INSERT INTO test VALUES (2, \'second insertion\');',
	stdout => \$stdout,
	stderr => \$stderr,
	timeout => $PostgreSQL::Test::Utils::timeout_default,
	timed_out => \$timed_out,
	on_error_die => 1,
	extra_params => ['--single-transaction']);

ok($cmdret == 0 && $timed_out == 0, 'Query works again');

test_sync_state(
	$node_primary,
	"${cascade_standby0_names[0]}|1|sync
${cascade_standby0_names[1]}|2|sync
${cascade_standby0_names[2]}|3|sync",
	'check that primary state backs to normal');

test_sync_state(
	$cascade_standbys0[0],
	"${cascade_standby1_names[0]}|1|quorum
${cascade_standby1_names[1]}|1|quorum
${cascade_standby1_names[2]}|0|async",
	'check that cascade standby state backs to normal');

test_sync_state($cascade_standbys1[0], "$standby_name|1|sync",
	'check that standby 5 is sync');

check_replication_state(
	$node_primary, $cascade_standbys0[0], $cascade_standbys0[1],
	$cascade_standbys0[2], $cascade_standbys0[3], 'primary_backup');

test_sync_state(
	$node_primary,
	"${cascade_standby0_names[0]}|1|sync
${cascade_standby0_names[1]}|2|sync
${cascade_standby0_names[2]}|3|potential
${cascade_standby0_names[3]}|0|async",
	'check that primary state backs to normal',
	"FIRST 2 (${cascade_standby0_names[0]}, ${cascade_standby0_names[1]}, ${cascade_standby0_names[2]})"
);

# Reorder standbys in WalSnd array after testing

$cascade_standbys1[0]->stop;
$cascade_standbys1[1]->stop;
$cascade_standbys1[2]->stop;

$cascade_standbys0[0]->stop;

start_standby_and_wait($node_primary, $cascade_standbys0[0]);
start_standby_and_wait($cascade_standbys0[0], $cascade_standbys1[0]);
start_standby_and_wait($cascade_standbys0[0], $cascade_standbys1[1]);
start_standby_and_wait($cascade_standbys0[0], $cascade_standbys1[2]);

check_replication_state(
	$cascade_standbys0[0], $cascade_standbys1[0],
	$cascade_standbys1[1], $cascade_standbys1[2],
	$cascade_standbys1[3], 'cascade_standby00_backup');

test_sync_state(
	$node_primary,
	"${cascade_standby0_names[0]}|1|sync
${cascade_standby0_names[1]}|2|sync
${cascade_standby0_names[2]}|3|potential
${cascade_standby0_names[3]}|0|async",
	'check that primary state does not change after testing cascade sync replication on standby 1'
);

# Reorder standbys in WalSnd array after testing

$standbys0[0]->stop;
$standbys0[1]->stop;
$standbys0[2]->stop;

$cascade_standbys0[1]->stop;

start_standby_and_wait($node_primary, $cascade_standbys0[1]);
start_standby_and_wait($cascade_standbys0[1], $standbys0[0]);
start_standby_and_wait($cascade_standbys0[1], $standbys0[1]);
start_standby_and_wait($cascade_standbys0[1], $standbys0[2]);

check_replication_state($cascade_standbys0[1], $standbys0[0], $standbys0[1],
	$standbys0[2], $standbys0[3], 'cascade_standby01_backup');

test_sync_state(
	$node_primary,
	"${cascade_standby0_names[0]}|1|sync
${cascade_standby0_names[1]}|2|sync
${cascade_standby0_names[2]}|3|potential
${cascade_standby0_names[3]}|0|async",
	'check that primary state does not change after testing cascade sync replication on standby 2'
);

# Reorder standbys in WalSnd array after testing

$standbys1[0]->stop;
$standbys1[1]->stop;
$standbys1[2]->stop;

$cascade_standbys0[2]->stop;

start_standby_and_wait($node_primary, $cascade_standbys0[2]);
start_standby_and_wait($cascade_standbys0[2], $standbys1[0]);
start_standby_and_wait($cascade_standbys0[2], $standbys1[1]);
start_standby_and_wait($cascade_standbys0[2], $standbys1[2]);

check_replication_state($cascade_standbys0[2], $standbys1[0], $standbys1[1],
	$standbys1[2], $standbys1[3], 'cascade_standby02_backup');

test_sync_state(
	$node_primary,
	"${cascade_standby0_names[0]}|1|sync
${cascade_standby0_names[1]}|2|sync
${cascade_standby0_names[2]}|3|potential
${cascade_standby0_names[3]}|0|async",
	'check that primary state does not change after testing cascade sync replication on standby 2'
);

done_testing();
