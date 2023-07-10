use strict;
use warnings;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;
use Test::More tests => 4;

my $sql_dir    = $ENV{PWD} . '/sql';
my $regress_db = 'postgres';

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);

$node_primary->append_conf('postgresql.conf', 'synchronous_commit=on');
$node_primary->append_conf('postgresql.conf', 'full_page_writes=off');
$node_primary->append_conf('postgresql.conf', 'log_min_messages=debug2');

$node_primary->start;

# create one table and insert some data
$node_primary->safe_psql(
	$regress_db,
	q(create table test (id int, name char(128) default 'A');insert into test select generate_series(1,64);));

# do one checkpoint to flush all the data and wal
$node_primary->safe_psql($regress_db, q(checkpoint;));

# insert more data
$node_primary->safe_psql(
	$regress_db,
	q(insert into test values (65);));

# check count(*) of table test
my $result = $node_primary->safe_psql($regress_db, q(select count(*) from test;));
is($result, '65', 'count of table test check before restart');

# get the file path of test
my $file_path = $node_primary->safe_psql($regress_db, q(select pg_relation_filepath('test');));
$file_path =~ s/^\s+|\s+$//g;
print "rel file path is $file_path\n";

# immediate stop primary
$node_primary->stop('i');

# truncate file to 8192
system_or_bail('truncate', '-s', '8192', $node_primary->data_dir . '/' . $file_path);

my $log_location = -s $node_primary->logfile;
# start
$node_primary->start;

$node_primary->log_check('find invalid page for rel file path', $log_location,
						 log_like => [qr/page [0-9]+ of relation $file_path does not exist/]);

$node_primary->log_check('find dropped invalid page for rel file path', $log_location,
						 log_like => [qr/page [0-9]+ of relation $file_path has been dropped/]);

# check count(*) of table test again
$result = $node_primary->safe_psql($regress_db, q(select count(*) from test;));
is($result, '64', 'count of table test check after restart');

$node_primary->stop();
