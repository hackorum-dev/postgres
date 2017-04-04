#!/usr/bin/env perl
# Demonstrate that logical can follow timeline switches.
#
# Test logical decoding on a standby.
#
use strict;
use warnings;
use 5.8.0;

use PostgresNode;
use TestLib;
use Test::More tests => 77;
use RecursiveCopy;
use File::Copy;

my ($stdin, $stdout, $stderr, $ret, $handle, $return);
my $backup_name;

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1, has_archiving => 1);
$node_master->append_conf('postgresql.conf', q{
wal_level = 'logical'
max_replication_slots = 4
max_wal_senders = 4
log_min_messages = 'debug2'
log_error_verbosity = verbose
# send status rapidly so we promptly advance xmin on master
wal_receiver_status_interval = 1
# very promptly terminate conflicting backends
max_standby_streaming_delay = '2s'
});
$node_master->dump_info;
$node_master->start;

$node_master->psql('postgres', q[CREATE DATABASE testdb]);

$node_master->safe_psql('testdb', q[SELECT * FROM pg_create_physical_replication_slot('decoding_standby');]);
$backup_name = 'b1';
my $backup_dir = $node_master->backup_dir . "/" . $backup_name;
TestLib::system_or_bail('pg_basebackup', '-D', $backup_dir, '-d', $node_master->connstr('testdb'), '--write-recovery-conf', '--slot=decoding_standby');

open(my $fh, "<", $backup_dir . "/recovery.conf")
  or die "can't open recovery.conf";

my $found = 0;
while (my $line = <$fh>)
{
	chomp($line);
	if ($line eq "primary_slot_name = 'decoding_standby'")
	{
		$found = 1;
		last;
	}
}
ok($found, "using physical slot for standby");

sub print_phys_xmin
{
	my $slot = $node_master->slot('decoding_standby');
	return ($slot->{'xmin'}, $slot->{'catalog_xmin'});
}

my ($xmin, $catalog_xmin) = print_phys_xmin();
# After slot creation, xmins must be null
is($xmin, '', "xmin null");
is($catalog_xmin, '', "catalog_xmin null");

my $node_replica = get_new_node('replica');
$node_replica->init_from_backup(
	$node_master, $backup_name,
	has_streaming => 1,
	has_restoring => 1);

$node_replica->start;
$node_master->wait_for_catchup($node_replica, 'replay', $node_master->lsn('flush'));

# with hot_standby_feedback off, xmin and catalog_xmin must still be null
($xmin, $catalog_xmin) = print_phys_xmin();
is($xmin, '', "xmin null after replica join");
is($catalog_xmin, '', "catalog_xmin null after replica join");

$node_replica->append_conf('postgresql.conf',q[
hot_standby_feedback = on
]);
$node_replica->restart;
sleep(2); # ensure walreceiver feedback sent

# If no slot on standby exists to hold down catalog_xmin it must follow xmin,
# (which is nextXid when no xacts are running on the standby).
($xmin, $catalog_xmin) = print_phys_xmin();
ok($xmin, "xmin not null");
is($xmin, $catalog_xmin, "xmin and catalog_xmin equal");

# We need catalog_xmin advance to take effect on the master and be replayed
# on standby.
$node_master->safe_psql('postgres', 'CHECKPOINT');
$node_master->wait_for_catchup($node_replica, 'replay', $node_master->lsn('flush'));

# Create new slots on the replica, ignoring the ones on the master completely.
#
# This must succeed since we know we have a catalog_xmin reservation. We
# might've already sent hot standby feedback to advance our physical slot's
# catalog_xmin but not received the corresponding xlog for the catalog xmin
# advance, in which case we'll create a slot that isn't usable. The calling
# application can prevent this by creating a temporary slot on the master to
# lock in its catalog_xmin. For a truly race-free solution we'd need
# master-to-standby hot_standby_feedback replies.
#
# In this case it won't race because there's no concurrent activity on the
# master.
#
is($node_replica->psql('testdb', qq[SELECT * FROM pg_create_logical_replication_slot('standby_logical', 'test_decoding')]),
   0, 'logical slot creation on standby succeeded')
	or BAIL_OUT('cannot continue if slot creation fails, see logs');

sub print_logical_xmin
{
	my $slot = $node_replica->slot('standby_logical');
	return ($slot->{'xmin'}, $slot->{'catalog_xmin'});
}

$node_master->wait_for_catchup($node_replica, 'replay', $node_master->lsn('flush'));
sleep(2); # ensure walreceiver feedback sent

($xmin, $catalog_xmin) = print_phys_xmin();
isnt($xmin, '', "physical xmin not null");
isnt($catalog_xmin, '', "physical catalog_xmin not null");

($xmin, $catalog_xmin) = print_logical_xmin();
is($xmin, '', "logical xmin null");
isnt($catalog_xmin, '', "logical catalog_xmin not null");

$node_master->safe_psql('testdb', 'CREATE TABLE test_table(id serial primary key, blah text)');
$node_master->safe_psql('testdb', q[INSERT INTO test_table(blah) values ('itworks')]);

$node_master->wait_for_catchup($node_replica, 'replay', $node_master->lsn('flush'));
sleep(2); # ensure walreceiver feedback sent

($xmin, $catalog_xmin) = print_phys_xmin();
isnt($xmin, '', "physical xmin not null");
isnt($catalog_xmin, '', "physical catalog_xmin not null");

$node_master->wait_for_catchup($node_replica, 'replay', $node_master->lsn('flush'));
sleep(2); # ensure walreceiver feedback sent

($ret, $stdout, $stderr) = $node_replica->psql('testdb', qq[SELECT data FROM pg_logical_slot_get_changes('standby_logical', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1', 'include-timestamp', '0')]);
is($stderr, '', 'stderr is empty');
is($ret, 0, 'replay from slot succeeded')
	or BAIL_OUT('cannot continue if slot replay fails');
is($stdout, q{BEGIN
table public.test_table: INSERT: id[integer]:1 blah[text]:'itworks'
COMMIT}, 'replay results match');

$node_master->wait_for_catchup($node_replica, 'replay', $node_master->lsn('flush'));
sleep(2); # ensure walreceiver feedback sent

my ($physical_xmin, $physical_catalog_xmin) = print_phys_xmin();
isnt($physical_xmin, '', "physical xmin not null");
isnt($physical_catalog_xmin, '', "physical catalog_xmin not null");

my ($logical_xmin, $logical_catalog_xmin) = print_logical_xmin();
is($logical_xmin, '', "logical xmin null");
isnt($logical_catalog_xmin, '', "logical catalog_xmin not null");

# Ok, do a pile of tx's and make sure xmin advances.
# Ideally we'd just hold catalog_xmin, but since hs_feedback currently uses the slot,
# we hold down xmin.
$node_master->safe_psql('testdb', qq[CREATE TABLE catalog_increase_1();]);
for my $i (0 .. 2000)
{
    $node_master->safe_psql('testdb', qq[INSERT INTO test_table(blah) VALUES ('entry $i')]);
}
$node_master->safe_psql('testdb', qq[CREATE TABLE catalog_increase_2();]);
$node_master->safe_psql('testdb', 'VACUUM');

my ($new_logical_xmin, $new_logical_catalog_xmin) = print_logical_xmin();
cmp_ok($new_logical_catalog_xmin, "==", $logical_catalog_xmin, "logical slot catalog_xmin hasn't advanced before get_changes");

($ret, $stdout, $stderr) = $node_replica->psql('testdb', qq[SELECT data FROM pg_logical_slot_get_changes('standby_logical', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1', 'include-timestamp', '0')]);
is($ret, 0, 'replay of big series succeeded');
isnt($stdout, '', 'replayed some rows');

($new_logical_xmin, $new_logical_catalog_xmin) = print_logical_xmin();
is($new_logical_xmin, '', "logical xmin null");
isnt($new_logical_catalog_xmin, '', "logical slot catalog_xmin not null");
cmp_ok($new_logical_catalog_xmin, ">", $logical_catalog_xmin, "logical slot catalog_xmin advanced after get_changes");

$node_master->wait_for_catchup($node_replica, 'replay', $node_master->lsn('flush'));
sleep(2); # ensure walreceiver feedback sent

$node_master->wait_for_catchup($node_replica, 'replay', $node_master->lsn('flush'));
sleep(2); # ensure walreceiver feedback sent

my ($new_physical_xmin, $new_physical_catalog_xmin) = print_phys_xmin();
isnt($new_physical_xmin, '', "physical xmin not null");
# hot standby feedback should advance phys catalog_xmin now the standby's slot
# doesn't hold it down as far.
isnt($new_physical_catalog_xmin, '', "physical catalog_xmin not null");
cmp_ok($new_physical_catalog_xmin, ">", $physical_catalog_xmin, "physical catalog_xmin advanced");

cmp_ok($new_physical_catalog_xmin, "<=", $new_logical_catalog_xmin, 'upstream physical slot catalog_xmin not past downstream catalog_xmin with hs_feedback on');

#########################################################
# Upstream catalog retention
#########################################################

sub test_catalog_xmin_retention()
{
	# First burn some xids on the master in another DB, so we push the master's
	# nextXid ahead.
	foreach my $i (1 .. 100)
	{
		$node_master->safe_psql('postgres', 'SELECT txid_current()');
	}

	# Force vacuum freeze on the master and ensure its oldestXmin doesn't advance
	# past our needed xmin. The only way we have visibility into that is to force
	# a checkpoint.
	$node_master->safe_psql('postgres', "UPDATE pg_database SET datallowconn = true WHERE datname = 'template0'");
	foreach my $dbname ('template1', 'postgres', 'testdb', 'template0')
	{
		$node_master->safe_psql($dbname, 'VACUUM FREEZE');
	}
	sleep(1);
	$node_master->safe_psql('postgres', 'CHECKPOINT');
	IPC::Run::run(['pg_controldata', $node_master->data_dir()], '>', \$stdout)
		or die "pg_controldata failed with $?";
	my @checkpoint = split('\n', $stdout);
	my ($oldestXid, $oldestCatalogXmin, $nextXid) = ('', '', '');
	foreach my $line (@checkpoint)
	{
		if ($line =~ qr/^Latest checkpoint's NextXID:\s+\d+:(\d+)/)
		{
			$nextXid = $1;
		}
		if ($line =~ qr/^Latest checkpoint's oldestXID:\s+(\d+)/)
		{
			$oldestXid = $1;
		}
		if ($line =~ qr/^Latest checkpoint's oldestCatalogXmin:\s*(\d+)/)
		{
			$oldestCatalogXmin = $1;
		}
	}
	die 'no oldestXID found in checkpoint' unless $oldestXid;

	my ($new_physical_xmin, $new_physical_catalog_xmin) = print_phys_xmin();
	my ($new_logical_xmin, $new_logical_catalog_xmin) = print_logical_xmin();

	print "upstream oldestXid $oldestXid, oldestCatalogXmin $oldestCatalogXmin, nextXid $nextXid, phys slot catalog_xmin $new_physical_catalog_xmin, downstream catalog_xmin $new_logical_catalog_xmin";

	$node_master->safe_psql('postgres', "UPDATE pg_database SET datallowconn = false WHERE datname = 'template0'");

	return ($oldestXid, $oldestCatalogXmin);
}

my ($oldestXid, $oldestCatalogXmin) = test_catalog_xmin_retention();

cmp_ok($oldestXid, "<=", $new_logical_catalog_xmin, 'upstream oldestXid not past downstream catalog_xmin with hs_feedback on');
cmp_ok($oldestCatalogXmin, ">=", $oldestXid, "oldestCatalogXmin >= oldestXid");
cmp_ok($oldestCatalogXmin, "<=", $new_logical_catalog_xmin,, "oldestCatalogXmin >= downstream catalog_xmin");

#########################################################
# Conflict with recovery: xmin cancels decoding session
#########################################################
#
# Start a transaction on the replica then perform work that should cause a
# recovery conflict with it. We'll check to make sure the client gets
# terminated with recovery conflict.
#
# Temporarily disable hs feedback so we can test recovery conflicts.
# It's fine to continue using a physical slot, the xmin should be
# cleared. We only check hot_standby_feedback when establishing
# a new decoding session so this approach circumvents the safeguards
# in place and forces a conflict.
#
# We'll also create an unrelated table so we can drop it later, making
# sure there are catalog changes to replay.
$node_master->safe_psql('testdb', 'CREATE TABLE dummy_table(blah integer)');

# Start pg_recvlogical before we turn off hs_feedback so its slot's
# catalog_xmin is above the downstream's catalog_threshold when we start
# decoding.
$handle = IPC::Run::start(['pg_recvlogical', '-d', $node_replica->connstr('testdb'), '-S', 'standby_logical', '-f', '-', '--no-loop', '--start'], '>', \$stdout, '2>', \$stderr);

$node_replica->safe_psql('postgres', 'ALTER SYSTEM SET hot_standby_feedback = off');
$node_replica->reload;

sleep(2);

($xmin, $catalog_xmin) = print_phys_xmin();
is($xmin, '', "physical xmin null after hs_feedback disabled");
is($catalog_xmin, '', "physical catalog_xmin null after hs_feedback disabled");

# Burn a bunch of XIDs and make sure upstream catalog_xmin is past what we'll
# need here
($oldestXid, $oldestCatalogXmin) = test_catalog_xmin_retention();
cmp_ok($oldestXid, ">", $new_logical_catalog_xmin, 'upstream oldestXid advanced past downstream catalog_xmin with hs_feedback off');
cmp_ok($oldestCatalogXmin, "==", 0, "oldestCatalogXmin = InvalidTransactionId with hs_feedback off");

# Make some data-only changes. We don't have a way to delay advance of the
# catalog_xmin threshold until catalog changes are made, now that our slot is
# no longer holding down catalog_xmin this will result in a recovery conflict.
$node_master->safe_psql('testdb', 'DELETE FROM test_table');
# Force a checkpoint to make sure catalog_xmin advances
$node_master->safe_psql('testdb', 'CHECKPOINT;');

$node_master->wait_for_catchup($node_replica, 'replay', $node_master->lsn('flush'));

$handle->pump;

is($node_replica->slot('standby_logical')->{'active_pid'}, '', 'pg_recvlogical no longer connected to slot');

# client died?
eval {
	$handle->finish;
};
$return = $?;
if ($return) {
	is($return, 256, "pg_recvlogical terminated by server on recovery conflict");
	like($stderr, qr/terminating connection due to conflict with recovery/, 'recvlogical recovery conflict errmsg');
	like($stderr, qr/requires catalog rows that will be removed/, 'pg_recvlogical exited with catalog_xmin conflict');
}
else
{
	fail("pg_recvlogical returned ok $return with stdout '$stdout', stderr '$stderr'");
}

# record the xmin when the conflicts arose
my ($conflict_xmin, $conflict_catalog_xmin) = print_logical_xmin();

#####################################################################
# Conflict with recovery: oldestCatalogXmin should be zero with no feedback
#####################################################################
#
# We cleared the catalog_xmin on the physical slot when hs feedback was turned
# off. There's no logical slot on the master. So oldestCatalogXmin must be
# zero.
#
$node_replica->safe_psql('postgres', 'CHECKPOINT');
command_like(['pg_controldata', $node_replica->data_dir], qr/^Latest checkpoint's oldestCatalogXmin:0$/m,
	"pg_controldata's oldestCatalogXmin is zero when hot standby feedback is off");

#####################################################################
# Conflict with recovery: refuse to run without hot_standby_feedback
#####################################################################
#
# When hot_standby_feedback is off, new connections should fail.
#

IPC::Run::run(['pg_recvlogical', '-d', $node_replica->connstr('testdb'), '-S', 'standby_logical', '-f', '-', '--no-loop', '--start'], '>', \$stdout, '2>', \$stderr);
is($?, 256, 'pg_recvlogical failed to connect to slot while hot_standby_feedback off');
like($stderr, qr/hot_standby_feedback/, 'recvlogical recovery conflict errmsg');

#####################################################################
# Conflict with recovery: catalog_xmin advance invalidates idle slot
#####################################################################
#
# The slot that pg_recvlogical was using before it was terminated
# should not accept new connections now, since its catalog_xmin
# is lower than the replica's threshold. Even once we re-enable
# hot_standby_feedback, the removed tuples won't somehow come back.
#

$node_replica->safe_psql('postgres', 'ALTER SYSTEM SET hot_standby_feedback = on');
$node_replica->reload;
# Wait until hot_standby_feedback is applied
sleep(2);
# make sure we see the effect promptly in xlog
$node_master->safe_psql('postgres', 'CHECKPOINT');
$node_master->wait_for_catchup($node_replica, 'replay', $node_master->lsn('flush'));
sleep(2);
($xmin, $catalog_xmin) = print_phys_xmin();
ok($xmin, 'xmin on phys slot non-null after re-establishing hot standby feedback');
ok($catalog_xmin, 'catalog_xmin on phys slot non-null after re-establishing hot standby feedback')
	or BAIL_OUT('further results meaningless if catalog_xmin not set on master');

# The walsender will clamp the catalog_xmin on the slot, so when the standby sends
# feedback with a too-old catalog_xmin the result will actually be limited to
# the safe catalog_xmin.
cmp_ok($catalog_xmin, ">=", $conflict_catalog_xmin,
	'phys slot catalog_xmin has not rewound to replica logical slot catalog_xmin');

print "catalog_xmin is $catalog_xmin";

$node_replica->safe_psql('postgres', 'CHECKPOINT');
command_like(['pg_controldata', $node_replica->data_dir], qr/^Latest checkpoint's oldestCatalogXmin:(?!$conflict_catalog_xmin)[^0][[:digit:]]*$/m,
	"pg_controldata's oldestCatalogXmin has not rewound to slot catalog_xmin")
	or BAIL_OUT('oldestCatalogXmin rewound, further tests are nonsensical');

my $timer = IPC::Run::timeout(120);
eval {
	IPC::Run::run(['pg_recvlogical', '-d', $node_replica->connstr('testdb'), '-S', 'standby_logical', '-f', '-', '--no-loop', '--start'],
		'>', \$stdout, '2>', \$stderr, $timer);
};
ok(!$timer->is_expired, 'pg_recvlogical exited not timed out');
is($?, 256, 'pg_recvlogical failed to connect to slot with past catalog_xmin');
like($stderr, qr/replication slot '.*' requires catalogs removed by master/, 'recvlogical recovery conflict errmsg');


##################################################
# Drop slot
##################################################
#
is($node_replica->safe_psql('postgres', 'SHOW hot_standby_feedback'), 'on', 'hs_feedback is on');

($xmin, $catalog_xmin) = print_phys_xmin();

# Make sure slots on replicas are droppable, and properly clear the upstream's xmin
$node_replica->psql('testdb', q[SELECT pg_drop_replication_slot('standby_logical')]);

is($node_replica->slot('standby_logical')->{'slot_type'}, '', 'slot on standby dropped manually');

$node_master->wait_for_catchup($node_replica, 'replay', $node_master->lsn('flush'));
sleep(2); # ensure walreceiver feedback sent

my ($new_xmin, $new_catalog_xmin) = print_phys_xmin();
# We're now back to the old behaviour of hot_standby_feedback
# reporting nextXid for both thresholds
ok($new_catalog_xmin, "physical catalog_xmin still non-null");
cmp_ok($new_catalog_xmin, '==', $new_xmin,
	'xmin and catalog_xmin equal after slot drop');


##################################################
# Recovery: drop database drops idle slots
##################################################

# Create a couple of slots on the DB to ensure they are dropped when we drop
# the DB on the upstream if they're on the right DB, or not dropped if on
# another DB.

$node_replica->command_ok(['pg_recvlogical', '-d', $node_replica->connstr('testdb'), '-P', 'test_decoding', '-S', 'dodropslot', '--create-slot'], 'pg_recvlogical created dodropslot')
	or BAIL_OUT('slot creation failed, subsequent results would be meaningless');
$node_replica->command_ok(['pg_recvlogical', '-d', $node_replica->connstr('postgres'), '-P', 'test_decoding', '-S', 'otherslot', '--create-slot'], 'pg_recvlogical created otherslot')
	or BAIL_OUT('slot creation failed, subsequent results would be meaningless');

is($node_replica->slot('dodropslot')->{'slot_type'}, 'logical', 'slot dodropslot on standby created');
is($node_replica->slot('otherslot')->{'slot_type'}, 'logical', 'slot otherslot on standby created');

# dropdb on the master to verify slots are dropped on standby
$node_master->safe_psql('postgres', q[DROP DATABASE testdb]);

$node_master->wait_for_catchup($node_replica, 'replay', $node_master->lsn('flush'));

is($node_replica->safe_psql('postgres', q[SELECT EXISTS(SELECT 1 FROM pg_database WHERE datname = 'testdb')]), 'f',
  'database dropped on standby');

is($node_replica->slot('dodropslot2')->{'slot_type'}, '', 'slot on standby dropped');
is($node_replica->slot('otherslot')->{'slot_type'}, 'logical', 'otherslot on standby not dropped');


##################################################
# Recovery: drop database drops in-use slots
##################################################

# This time, have the slot in-use on the downstream DB when we drop it.
print "Testing dropdb when downstream slot is in-use";
$node_master->psql('postgres', q[CREATE DATABASE testdb2]);

print "creating slot dodropslot2";
$node_replica->command_ok(['pg_recvlogical', '-d', $node_replica->connstr('testdb2'), '-P', 'test_decoding', '-S', 'dodropslot2', '--create-slot'],
	'pg_recvlogical created slot test_decoding');
is($node_replica->slot('dodropslot2')->{'slot_type'}, 'logical', 'slot dodropslot2 on standby created');

# make sure the slot is in use
print "starting pg_recvlogical";
$handle = IPC::Run::start(['pg_recvlogical', '-d', $node_replica->connstr('testdb2'), '-S', 'dodropslot2', '-f', '-', '--no-loop', '--start'], '>', \$stdout, '2>', \$stderr);
sleep(1);

is($node_replica->slot('dodropslot2')->{'active'}, 't', 'slot on standby is active')
  or BAIL_OUT("slot not active on standby, cannot continue. pg_recvlogical exited with '$stdout', '$stderr'");

# Master doesn't know the replica's slot is busy so dropdb should succeed
$node_master->safe_psql('postgres', q[DROP DATABASE testdb2]);
ok(1, 'dropdb finished');

while ($node_replica->slot('dodropslot2')->{'active_pid'})
{
	sleep(1);
	print "waiting for walsender to exit";
}

print "walsender exited, waiting for pg_recvlogical to exit";

# our client should've terminated in response to the walsender error
eval {
	$handle->finish;
};
$return = $?;
if ($return) {
	is($return, 256, "pg_recvlogical terminated by server");
	like($stderr, qr/terminating connection due to conflict with recovery/, 'recvlogical recovery conflict');
	like($stderr, qr/User was connected to a database that must be dropped./, 'recvlogical recovery conflict db');
}

is($node_replica->slot('dodropslot2')->{'active_pid'}, '', 'walsender backend exited');

# The slot should be dropped by recovery now
$node_master->wait_for_catchup($node_replica, 'replay', $node_master->lsn('flush'));

is($node_replica->safe_psql('postgres', q[SELECT EXISTS(SELECT 1 FROM pg_database WHERE datname = 'testdb2')]), 'f',
  'database dropped on standby');

is($node_replica->slot('dodropslot2')->{'slot_type'}, '', 'slot on standby dropped');
