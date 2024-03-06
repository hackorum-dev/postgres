# Copyright (c) 2021-2023, PostgreSQL Global Development Group

# Basic logical replication test
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf('postgresql.conf', 'logical_decoding_work_mem = 64kB');
$node_publisher->append_conf('postgresql.conf', 'log_min_messages = DEBUG1');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;


# Create some preexisting content on publisher
$node_publisher->safe_psql('postgres',
	"create table tbl_pub(id int, val1 text, val2 text,size text);");
$node_publisher->safe_psql('postgres',
	"create table tbl_t1(id int, val1 text, val2 text,size text);");
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION mypub FOR TABLE public.tbl_pub;");
$node_publisher->safe_psql('postgres',
qq(
CREATE OR REPLACE FUNCTION check_replication_status() RETURNS VOID AS \$\$
DECLARE
    replication_record pg_stat_replication;
BEGIN
    LOOP
        SELECT *
        INTO replication_record
        FROM pg_stat_replication
        WHERE application_name = 'mysub';
        
        IF replication_record.replay_lsn = replication_record.write_lsn THEN
            EXIT;
        END IF;
    
        PERFORM pg_sleep(1);
    END LOOP;
END;
\$\$ LANGUAGE plpgsql;));

# Create some preexisting content on subscriber
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_subscriber->safe_psql('postgres', 
    "create table tbl_pub(id int, val1 text, val2 text,size text);");
$node_subscriber->safe_psql('postgres',
    "create table tbl_t1(id int, val1 text, val2 text,size text);");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION mysub CONNECTION '$publisher_connstr' PUBLICATION mypub"
);

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'mysub');

# test history snapshot
$node_publisher->safe_psql('postgres',
	"insert into tbl_pub select i, 'xyzzy', 'abcba', 'truncated' from generate_series(1,9) i;truncate tbl_pub;");
$node_publisher->safe_psql('postgres',
	"insert into tbl_pub select i, 'xyzzy', 'abcba', 'truncated' from generate_series(1,9) i;");
my $pub_count =   $node_publisher->safe_psql('postgres',
	"select count(*) from tbl_pub;");
is($pub_count, 9, 'check that the historical snapshot is correct.');

# test table change filter
my $logstart = -s $node_publisher->logfile;
$node_publisher->safe_psql('postgres',
qq(BEGIN;
insert into tbl_t1 select 1, 'xyzzy', 'abcba', sum(size) from pg_ls_replslotdir('mysub');
insert into tbl_t1 select i,repeat('xyzzy', i),repeat('abcba',i),(select sum(size) from pg_ls_replslotdir('mysub')) from generate_series(2,99) i;
update tbl_t1 set val2 = repeat('xyzzy',id) where id > 1 and id < 10001;
select check_replication_status();
insert into tbl_t1 select 10001, 'xyzzy', 'abcba', sum(size) from pg_ls_replslotdir('mysub');
COMMIT;)
);

my $filter_table_oid =  $node_publisher->safe_psql('postgres', "select oid from pg_class where relname='tbl_t1';");
ok($node_publisher->log_contains("logical filter change by table " . $filter_table_oid, $logstart),
	"the change of the tbl_t1 table is filtered.");

done_testing();