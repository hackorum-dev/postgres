# Test testing indexes replication
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 1;

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1);
$node_master->start;

# Add a table for gist index check
$node_master->safe_psql('postgres',
    qq{
create table gist_point_tbl(id int, p point);
insert into gist_point_tbl(id, p)
    select g, point(g*10, g*10) from generate_series(10000, 11000) g;
create index gist_pointidx on gist_point_tbl using gist(p);});

# Take backup
my $backup_name = 'my_backup';
$node_master->backup($backup_name);

# Create streaming standby from backup
my $node_standby = get_new_node('standby');
$node_standby->init_from_backup($node_master, $backup_name,
    has_streaming => 1);
$node_standby->start;

$node_master->safe_psql('postgres', qq{
create temp table gist_point_tbl_t(id int, p point);
create index gist_pointidx_t on gist_point_tbl_t using gist(p);
insert into gist_point_tbl_t(id, p)
    select g, point(g*10, g*10) from generate_series(1, 1000) g;
set enable_seqscan=off;
set enable_bitmapscan=off;
explain (costs off)
select p from gist_point_tbl_t where p <@ box(point(0,0), point(100, 100));
});

$node_master->safe_psql('postgres', qq{
create unlogged table gist_point_tbl_u(id int, p point);
create index gist_pointidx_u on gist_point_tbl_u using gist(p);
insert into gist_point_tbl_u(id, p)
    select g, point(g*10, g*10) from generate_series(1, 1000) g;
set enable_seqscan=off;
set enable_bitmapscan=off;
explain (costs off)
select p from gist_point_tbl_u where p <@ box(point(0,0), point(100, 100));
});

$node_master->safe_psql('postgres', qq{
insert into gist_point_tbl (id, p)
    select g, point(g*10, g*10) from generate_series(1, 1000) g;});

$node_master->safe_psql('postgres', "delete from gist_point_tbl where id < 500");

$node_master->safe_psql('postgres', qq{
create table test as (select x, box(point(x, x),point(x, x))
   from generate_series(1,4000000) as x);

create index test_idx on test using gist (box);

set enable_seqscan TO false;
set enable_bitmapscan TO false;

delete from test where box && box(point(0,0), point(100000,100000));

-- This query invokes gistkillitems()
select count(box) from test where box && box(point(0,0), point(100000,100000));
});

$node_master->safe_psql('postgres', qq{
insert into test
    select x, box(point(x, x),point(x*3, x*3))
    from generate_series(1,200000) as x;});

$node_master->safe_psql('postgres', qq{
update test set box = box where x<2000000; vacuum test;});
$node_master->safe_psql('postgres', qq{
delete from test where x<1000000; vacuum test;});
$node_master->safe_psql('postgres', qq{
insert into test
    select x, box(point(x, x),point(x, x))
    from generate_series(1,1000000) as x;});

$node_master->safe_psql('postgres', qq{
insert into gist_point_tbl (id, p)
    select g,        point(g*5, g*5) from generate_series(1, 10000) g;});

$node_master->wait_for_catchup($node_standby, 'replay');

my $result = $node_standby->safe_psql('postgres', qq{
set enable_seqscan=off;
set enable_bitmapscan=off;
explain (costs off)
select p from gist_point_tbl where p <@ box(point(0,0), point(100, 100));});
ok($result =~ /^Index Only Scan using gist_pointidx/, "gist index used on a standby");

#$node_standby->stop();
#$node_master->stop();
