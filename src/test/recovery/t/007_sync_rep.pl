# Minimal test testing synchronous replication sync_state transition
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 15;

use IPC::Run qw( start pump finish );

# Query checking sync_priority and sync_state of each standby
my $check_sql =
  "SELECT application_name, sync_priority, sync_state FROM pg_stat_replication ORDER BY application_name;";

# Check that sync_state of each standby is expected (waiting till it is).
# If $setting is given, synchronous_standby_names is set to it and
# the configuration file is reloaded before the test.
sub test_sync_state
{
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
	my ($master, $standby) = @_;
	my $master_name  = $master->name;
	my $standby_name = $standby->name;
	my $query =
	  "SELECT count(1) = 1 FROM pg_stat_replication WHERE application_name = '$standby_name'";

	$standby->start;

	print("### Waiting for standby \"$standby_name\" on \"$master_name\"\n");
	$master->poll_query_until('postgres', $query);
	return;
}

sub startpsql
{
	my ($host, $port) = @_;
	unless (defined($host) && defined($port))
	{
		die "host and port must be specified";
	}

	my %ret;
	my $in;
	my $out;
	my $err;
	my $harness;
	my @psql = qw( psql -d postgres -h );
	$psql[++$#psql] = $host;
	$psql[++$#psql] = '-p';
	$psql[++$#psql] = $port;

	$ret{"harness"} = start \@psql, \$in, \$out, \$err;
	$ret{"in"} = \$in;
	$ret{"out"} = \$out;
	$ret{"err"} = \$err;
	return \%ret;
}

sub sendSQL
{
	my $session = $_[0];
	my $outref = $session->{out};
	my $errref = $session->{err};

	# Reset output and error buffers so that they will only contain
	# the results of this SQL command.
	$$outref = "";
	$$errref = "";

	# Assigning the SQL statement to $inref causes it to be sent to
	# the psql child process.
	my $inref = $session->{in};
	$$inref = $_[1];

	pump $session->{harness} while length $$inref;
}

sub getResults
{
	my $session = $_[0];
	my $inref = $session->{in};
	my $outref = $session->{out};
	my $errref = $session->{err};

	while ($$outref !~ /$_[1]/ && $$errref !~ /ERR/)
	{
		pump $session->{harness};
	}
	die "psql failed:\n", $$errref if length $$errref;
	return $$outref;
}

# This test injects a fault in a standby by invoking faultinjector
# interface on master.  The fault causes standby to respond with stale
# flush LSN value, simulating the case that it has not caught up.  If
# the standby is synchronous, commits on master should wait until
# standby confirms it has flush WAL greater than or up to commit LSN.
sub test_sync_commit
{
	my ($master, $standby) = @_;

	# inject fault remotely on standby1 such that it replies with the same
	# LSN as the last time, in spite of having flushed newer WAL records
	# received from master.
	my ($cmdret, $stdout, $stderr) =
	  $master->psql('postgres', 'create extension faultinjector;', on_error_die => 1);
	
	my $sql = sprintf(
		"select inject_fault_infinite('standby_flush', 'skip', '%s', %d)",
		$standby->host, $standby->port);
	($cmdret, $stdout, $stderr) = $master->psql('postgres', $sql);
	ok($stdout =~ /Success/, 'inject skip fault in standby');

 	# commit a transaction on master, the master backend should wait
 	# because standby1 hasn't acknowledged the receipt of the commit LSN.
 	my $background_psql = startpsql($master->host, $master->port);
 	sendSQL $background_psql, "create table test_sync_commit(a int);\n";

	# Checkpoint so as to advance sent_lsn.  Due to the fault,
	# flush_lsn should remain unchanged.
	($cmdret, $stdout, $stderr) =
	  $master->psql('postgres', 'checkpoint;', on_error_die => 1);
	($cmdret, $stdout, $stderr) =
	  $master->psql(
		  'postgres',
		  qq(select case when sent_lsn > flush_lsn then 'Success'
 else 'Failure' end from pg_stat_replication),
		  on_error_die => 1);
	ok($stdout =~ /Success/, 'master WAL has moved ahead of standby');

	# Verify that the create table transaction started in background
	# is waiting for sync rep.
	($cmdret, $stdout, $stderr) =
	  $master->psql(
		  'postgres',
		  qq(select query from pg_stat_activity where wait_event = 'SyncRep'),
		  on_error_die => 1);
	ok($stdout =~ /create table test_sync_commit/, 'commit waits for standby');

	# Remove the fault from standby so that it starts responding with
	# the real write and flush LSN values.
	$sql =~ s/skip/reset/;
	$sql =~ s/_infinite//;
	($cmdret, $stdout, $stderr) = $master->psql('postgres', $sql);
	ok($stdout =~ /Success/, ' fault removed from standby');

	# Wait for the create table transaction to commit.
	getResults($background_psql, 'CREATE TABLE');
}

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1);
$node_master->start;
my $backup_name = 'master_backup';

# Take backup
$node_master->backup($backup_name);

# Create all the standbys.  Their status on the primary is checked to ensure
# the ordering of each one of them in the WAL sender array of the primary.

# Create standby1 linking to master
my $node_standby_1 = get_new_node('standby1');
$node_standby_1->init_from_backup($node_master, $backup_name,
	has_streaming => 1);
start_standby_and_wait($node_master, $node_standby_1);

# Create standby2 linking to master
my $node_standby_2 = get_new_node('standby2');
$node_standby_2->init_from_backup($node_master, $backup_name,
	has_streaming => 1);
start_standby_and_wait($node_master, $node_standby_2);

# Create standby3 linking to master
my $node_standby_3 = get_new_node('standby3');
$node_standby_3->init_from_backup($node_master, $backup_name,
	has_streaming => 1);
start_standby_and_wait($node_master, $node_standby_3);

# Check that sync_state is determined correctly when
# synchronous_standby_names is specified in old syntax.
test_sync_state(
	$node_master, qq(standby1|1|sync
standby2|2|potential
standby3|0|async),
	'old syntax of synchronous_standby_names',
	'standby1,standby2');

# Check that all the standbys are considered as either sync or
# potential when * is specified in synchronous_standby_names.
# Note that standby1 is chosen as sync standby because
# it's stored in the head of WalSnd array which manages
# all the standbys though they have the same priority.
test_sync_state(
	$node_master, qq(standby1|1|sync
standby2|1|potential
standby3|1|potential),
	'asterisk in synchronous_standby_names',
	'*');

# Now that standby1 is considered synchronous, check if commits made
# on master wait for standby1 to catch up.
test_sync_commit($node_master, $node_standby_1);

# Stop and start standbys to rearrange the order of standbys
# in WalSnd array. Now, if standbys have the same priority,
# standby2 is selected preferentially and standby3 is next.
$node_standby_1->stop;
$node_standby_2->stop;
$node_standby_3->stop;

# Make sure that each standby reports back to the primary in the wanted
# order.
start_standby_and_wait($node_master, $node_standby_2);
start_standby_and_wait($node_master, $node_standby_3);

# Specify 2 as the number of sync standbys.
# Check that two standbys are in 'sync' state.
test_sync_state(
	$node_master, qq(standby2|2|sync
standby3|3|sync),
	'2 synchronous standbys',
	'2(standby1,standby2,standby3)');

# Start standby1
start_standby_and_wait($node_master, $node_standby_1);

# Create standby4 linking to master
my $node_standby_4 = get_new_node('standby4');
$node_standby_4->init_from_backup($node_master, $backup_name,
	has_streaming => 1);
$node_standby_4->start;

# Check that standby1 and standby2 whose names appear earlier in
# synchronous_standby_names are considered as sync. Also check that
# standby3 appearing later represents potential, and standby4 is
# in 'async' state because it's not in the list.
test_sync_state(
	$node_master, qq(standby1|1|sync
standby2|2|sync
standby3|3|potential
standby4|0|async),
	'2 sync, 1 potential, and 1 async');

# Check that sync_state of each standby is determined correctly
# when num_sync exceeds the number of names of potential sync standbys
# specified in synchronous_standby_names.
test_sync_state(
	$node_master, qq(standby1|0|async
standby2|4|sync
standby3|3|sync
standby4|1|sync),
	'num_sync exceeds the num of potential sync standbys',
	'6(standby4,standby0,standby3,standby2)');

# The setting that * comes before another standby name is acceptable
# but does not make sense in most cases. Check that sync_state is
# chosen properly even in case of that setting. standby1 is selected
# as synchronous as it has the highest priority, and is followed by a
# second standby listed first in the WAL sender array, which is
# standby2 in this case.
test_sync_state(
	$node_master, qq(standby1|1|sync
standby2|2|sync
standby3|2|potential
standby4|2|potential),
	'asterisk before another standby name',
	'2(standby1,*,standby2)');

# Check that the setting of '2(*)' chooses standby2 and standby3 that are stored
# earlier in WalSnd array as sync standbys.
test_sync_state(
	$node_master, qq(standby1|1|potential
standby2|1|sync
standby3|1|sync
standby4|1|potential),
	'multiple standbys having the same priority are chosen as sync',
	'2(*)');

# Stop Standby3 which is considered in 'sync' state.
$node_standby_3->stop;

# Check that the state of standby1 stored earlier in WalSnd array than
# standby4 is transited from potential to sync.
test_sync_state(
	$node_master, qq(standby1|1|sync
standby2|1|sync
standby4|1|potential),
	'potential standby found earlier in array is promoted to sync');

# Check that standby1 and standby2 are chosen as sync standbys
# based on their priorities.
test_sync_state(
	$node_master, qq(standby1|1|sync
standby2|2|sync
standby4|0|async),
	'priority-based sync replication specified by FIRST keyword',
	'FIRST 2(standby1, standby2)');

# Check that all the listed standbys are considered as candidates
# for sync standbys in a quorum-based sync replication.
test_sync_state(
	$node_master, qq(standby1|1|quorum
standby2|1|quorum
standby4|0|async),
	'2 quorum and 1 async',
	'ANY 2(standby1, standby2)');

# Start Standby3 which will be considered in 'quorum' state.
$node_standby_3->start;

# Check that the setting of 'ANY 2(*)' chooses all standbys as
# candidates for quorum sync standbys.
test_sync_state(
	$node_master, qq(standby1|1|quorum
standby2|1|quorum
standby3|1|quorum
standby4|1|quorum),
	'all standbys are considered as candidates for quorum sync standbys',
	'ANY 2(*)');
