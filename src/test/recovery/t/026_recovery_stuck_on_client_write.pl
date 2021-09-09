# Test for backend process gets stuck on client_write
use strict;
use warnings;

use PostgresNode;
use TestLib;
use IPC::Run;
use Test::More tests=>5;

# primary node
my $node_primary = PostgresNode->new('primary');
$node_primary->init(
	allows_streaming => 1,
	auth_extra       => [ '--create-role', 'repl_role' ]);
$node_primary->start;

# standby node
my $backup_name = 'my_backup';
# Take backup
$node_primary->backup($backup_name);
# Create streaming standby linking to primary
my $node_standby = PostgresNode->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
# start standby
$node_standby->append_conf('postgresql.conf', "max_standby_streaming_delay = 5000");
$node_standby->append_conf('postgresql.conf', "max_standby_archive_delay = 5000");
$node_standby->append_conf('postgresql.conf', "hot_standby_feedback = off");
$node_standby->append_conf('postgresql.conf', "logging_collector = on");
$node_standby->append_conf('postgresql.conf', "log_directory = 'log'");
$node_standby->start;

# Wait for standbys to catch up
$node_primary->safe_psql('postgres', 'CREATE TABLE test_table(val integer);');
# insert more data so that the output buffer can be filled up when select from the table
$node_primary->safe_psql('postgres', "INSERT INTO test_table(val) SELECT generate_series(1,10000000) as newwal");
$node_primary->safe_psql('postgres', "checkpoint;");
$node_primary->wait_for_catchup($node_standby, 'replay', $node_primary->lsn('insert'));
my $result = $node_standby->safe_psql('postgres', "SELECT count(*) FROM test_table");
print "standby result: $result\n";
ok($result == 10000000, 'check streamed content on standby');

my $connstr = $node_primary->connstr;
my @res = `nohup psql -d "$connstr" -c "begin; select pg_sleep(2); select * from test_table;" > tmp_check/myout.file 2>&1 &`;
my $host = $node_primary->host;
my $port = $node_primary->port;
my $client = readpipe("ps -ef | grep \"psql\" | grep \"$host\" | grep \"$port\" | grep -v grep | awk '{print \$2}'");
# stop client and backend will get stuck on secure_write()
print "ready to stop primary client pid: $client\n";
sleep 1;
@res = `kill -stop $client`;
my $backend = $node_primary->find_child("SELECT");
print "backend pid:$backend\n";

# cancel the query
sleep 2;
$node_primary->safe_psql('postgres', "select pg_cancel_backend($backend)");
# check the query
my $pid_new = $node_primary->find_child("SELECT");
print "backend pid:$pid_new\n";
ok($pid_new == $backend, "backend can't be canceled");
@res = `kill -cont $client`;
@res = `kill -s 9 $client`;

# create recovery conflict
$connstr = $node_standby->connstr;
@res = `nohup psql -d "$connstr" -c "begin; select pg_sleep(2); select * from test_table;" > tmp_check/myout.file 2>&1 &`;
$host = $node_standby->host;
$port = $node_standby->port;
$client = readpipe("ps -ef | grep \"psql\" | grep \"$host\" | grep \"$port\" | grep -v grep | awk '{print \$2}'");
print "ready to stop standby client pid: $client\n";
sleep 1;
@res = `kill -stop $client`;

# delete data and do vacuum in primary node
$node_primary->safe_psql("postgres", "delete from test_table");
$node_primary->safe_psql("postgres", "vacuum test_table");
# cancel the query
$backend = $node_standby->find_child("SELECT");
$node_standby->safe_psql('postgres', "select pg_cancel_backend($backend)");
# check the query
sleep 2;
$pid_new = $node_standby->find_child("SELECT");
print "backend pid:$pid_new\n";
ok($pid_new == $backend, "backend can't be canceled by user");

# wait until recovery conflict generate
sleep 3;
$pid_new = $node_standby->find_child("SELECT");
print "backend pid:$pid_new\n";
ok($pid_new == 0, "backend is canceled");
@res = `kill -cont $client`;
@res = `kill -s 9 $client`;

my $basedir = $node_standby->basedir();
my $logdir = "$basedir/pgdata/log";
my @exits = `grep -rn "terminating connection due to conflict with recovery" $logdir`;
my $found = @exits;
ok($found == 1, "backend is canceled due to recovery conflict");

$node_standby->stop;
$node_primary->stop;
