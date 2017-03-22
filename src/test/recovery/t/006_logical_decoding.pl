# Testing of logical decoding using SQL interface and/or pg_recvlogical
#
# Most logical decoding tests are in contrib/test_decoding. This module
# is for work that doesn't fit well there, like where server restarts
# are required.
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 44;

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1);
$node_master->append_conf('postgresql.conf', qq(
wal_level = logical
hot_standby_feedback = on
wal_receiver_status_interval = 1
log_min_messages = debug1
));
$node_master->start;

# Set up some changes before we make base backups
$node_master->safe_psql('postgres', qq[CREATE TABLE decoding_test(x integer, y text);]);

$node_master->safe_psql('postgres', qq[SELECT pg_create_logical_replication_slot('test_slot', 'test_decoding');]);

$node_master->safe_psql('postgres', qq[INSERT INTO decoding_test(x,y) SELECT s, s::text FROM generate_series(1,10) s;]);

# Launch two streaming replicas, one with and one without
# physical replication slots. We'll use these for tests
# involving interaction of logical and physical standby.
#
# Both backups are created with pg_basebackup.
#
my $backup_name = 'master_backup';
$node_master->backup($backup_name);

$node_master->safe_psql('postgres', q[SELECT pg_create_physical_replication_slot('slot_replica');]);
my $node_slot_replica = get_new_node('slot_replica');
$node_slot_replica->init_from_backup($node_master, $backup_name, has_streaming => 1);
$node_slot_replica->append_conf('recovery.conf', "primary_slot_name = 'slot_replica'");

my $node_noslot_replica = get_new_node('noslot_replica');
$node_noslot_replica->init_from_backup($node_master, $backup_name, has_streaming => 1);

$node_slot_replica->start;
$node_noslot_replica->start;

sub restartpoint_standbys
{
	# Force restartpoints to update control files on replicas
	$node_slot_replica->safe_psql('postgres', 'CHECKPOINT');
	$node_noslot_replica->safe_psql('postgres', 'CHECKPOINT');
}

sub wait_standbys
{
	my $lsn = $node_master->lsn('insert');
	$node_master->wait_for_catchup($node_noslot_replica, 'replay', $lsn);
	$node_master->wait_for_catchup($node_slot_replica, 'replay', $lsn);
}

# pg_basebackup doesn't copy replication slots
is($node_slot_replica->slot('test_slot')->{'slot_name'}, undef,
	'logical slot test_slot on master not copied by pg_basebackup');

# Make sure oldestCatalogXmin lands in the control file on master
$node_master->safe_psql('postgres', 'VACUUM;');
$node_master->safe_psql('postgres', 'CHECKPOINT;');

my @nodes = ($node_master, $node_slot_replica, $node_noslot_replica);

wait_standbys();
restartpoint_standbys();
foreach my $node (@nodes)
{
	# Master had an oldestCatalogXmin, so we must've inherited it via checkpoint
	command_like(['pg_controldata', $node->data_dir], qr/^Latest checkpoint's oldestCatalogXmin:[^0][\d]*$/m,
		"pg_controldata's oldestCatalogXmin is nonzero after start on " . $node->name);
}

# Basic decoding works
my($result) = $node_master->safe_psql('postgres', qq[SELECT pg_logical_slot_get_changes('test_slot', NULL, NULL);]);
is(scalar(my @foobar = split /^/m, $result), 12, 'Decoding produced 12 rows inc BEGIN/COMMIT');

# If we immediately crash the server we might lose the progress we just made
# and replay the same changes again. But a clean shutdown should never repeat
# the same changes when we use the SQL decoding interface.
$node_master->restart('fast');

# There are no new writes, so the result should be empty.
$result = $node_master->safe_psql('postgres', qq[SELECT pg_logical_slot_get_changes('test_slot', NULL, NULL);]);
chomp($result);
is($result, '', 'Decoding after fast restart repeats no rows');

# Insert some rows and verify that we get the same results from pg_recvlogical
# and the SQL interface.
$node_master->safe_psql('postgres', qq[INSERT INTO decoding_test(x,y) SELECT s, s::text FROM generate_series(1,4) s;]);

my $expected = q{BEGIN
table public.decoding_test: INSERT: x[integer]:1 y[text]:'1'
table public.decoding_test: INSERT: x[integer]:2 y[text]:'2'
table public.decoding_test: INSERT: x[integer]:3 y[text]:'3'
table public.decoding_test: INSERT: x[integer]:4 y[text]:'4'
COMMIT};

my $stdout_sql = $node_master->safe_psql('postgres', qq[SELECT data FROM pg_logical_slot_peek_changes('test_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');]);
is($stdout_sql, $expected, 'got expected output from SQL decoding session');

my $endpos = $node_master->safe_psql('postgres', "SELECT location FROM pg_logical_slot_peek_changes('test_slot', NULL, NULL) ORDER BY location DESC LIMIT 1;");
print "waiting to replay $endpos\n";

my $stdout_recv = $node_master->pg_recvlogical_upto('postgres', 'test_slot', $endpos, 10, 'include-xids' => '0', 'skip-empty-xacts' => '1');
chomp($stdout_recv);
is($stdout_recv, $expected, 'got same expected output from pg_recvlogical decoding session');

$stdout_recv = $node_master->pg_recvlogical_upto('postgres', 'test_slot', $endpos, 10, 'include-xids' => '0', 'skip-empty-xacts' => '1');
chomp($stdout_recv);
is($stdout_recv, '', 'pg_recvlogical acknowledged changes, nothing pending on slot');

# Create a second DB we'll use for testing dropping and accessing slots across
# databases. This matters since logical slots are globally visible objects that
# can only actually be used on one DB for most purposes.
$node_master->safe_psql('postgres', 'CREATE DATABASE otherdb');

is($node_master->psql('otherdb', "SELECT location FROM pg_logical_slot_peek_changes('test_slot', NULL, NULL) ORDER BY location DESC LIMIT 1;"), 3,
	'replaying logical slot from another database fails');

$node_master->safe_psql('otherdb', qq[SELECT pg_create_logical_replication_slot('otherdb_slot', 'test_decoding');]);

# make sure you can't drop a slot while active
my $pg_recvlogical = IPC::Run::start(['pg_recvlogical', '-d', $node_master->connstr('otherdb'), '-S', 'otherdb_slot', '-f', '-', '--start']);
$node_master->poll_query_until('otherdb', "SELECT EXISTS (SELECT 1 FROM pg_replication_slots WHERE slot_name = 'otherdb_slot' AND active_pid IS NOT NULL)");
is($node_master->psql('postgres', 'DROP DATABASE otherdb'), 3,
	'dropping a DB with inactive logical slots fails');
$pg_recvlogical->kill_kill;
is($node_master->slot('otherdb_slot')->{'slot_name'}, undef,
	'logical slot still exists');

$node_master->poll_query_until('otherdb', "SELECT EXISTS (SELECT 1 FROM pg_replication_slots WHERE slot_name = 'otherdb_slot' AND active_pid IS NULL)");
is($node_master->psql('postgres', 'DROP DATABASE otherdb'), 0,
	'dropping a DB with inactive logical slots succeeds');
is($node_master->slot('otherdb_slot')->{'slot_name'}, undef,
	'logical slot was actually dropped with DB');

# Restarting a node with wal_level = logical that has existing
# slots must succeed, but decoding from those slots must fail.
$node_master->safe_psql('postgres', 'ALTER SYSTEM SET wal_level = replica');
is($node_master->safe_psql('postgres', 'SHOW wal_level'), 'logical', 'wal_level is still logical before restart');
$node_master->restart;
is($node_master->safe_psql('postgres', 'SHOW wal_level'), 'replica', 'wal_level is replica');
isnt($node_master->slot('test_slot')->{'catalog_xmin'}, '0',
	'restored slot catalog_xmin is nonzero');
is($node_master->psql('postgres', qq[SELECT pg_logical_slot_get_changes('test_slot', NULL, NULL);]), 3,
	'reading from slot with wal_level < logical fails');
wait_standbys();
restartpoint_standbys();
foreach my $node (@nodes)
{
	command_like(['pg_controldata', $node->data_dir], qr/^Latest checkpoint's oldestCatalogXmin:[^0][\d]*$/m,
		"pg_controldata's oldestCatalogXmin is nonzero on " . $node->name);
}

# Dropping the slot must clear catalog_xmin
is($node_master->psql('postgres', q[SELECT pg_drop_replication_slot('test_slot')]), 0,
	'can drop logical slot while wal_level = replica');
is($node_master->slot('test_slot')->{'catalog_xmin'}, '', 'slot was dropped');
$node_master->safe_psql('postgres', 'VACUUM;');
$node_master->safe_psql('postgres', 'CHECKPOINT;');
wait_standbys();
restartpoint_standbys();
foreach my $node (@nodes)
{
	command_like(['pg_controldata', $node->data_dir], qr/^Latest checkpoint's oldestCatalogXmin:0$/m,
		"pg_controldata's oldestCatalogXmin is zero after drop, vacuum and checkpoint on " . $node->name);
}

foreach my $node (@nodes)
{
	$node->stop;
}
