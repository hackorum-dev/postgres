use strict;
use warnings;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;
use Test::More tests => 2;

sub kill_process
{
	my ($ppid, $process_name) = @_;
	my @childs = `ps -o pid,cmd --ppid $ppid` or die "command failed!";
	my $pid = 0;
	foreach my $child (@childs)
	{
		if (index($child, $process_name) > 0)
		{
			($pid, ) = split(' ', $child);
			print "## kill process ($child) with signal 19\n";
			system_or_bail('kill', '-19', $pid);
			last;
		}
	}
	die "Not find process $process_name\n" unless $pid > 0;
}

my $sql_dir    = $ENV{PWD} . '/sql';
my $regress_db = 'postgres';

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);

$node_primary->append_conf('postgresql.conf', 'synchronous_commit=on');
$node_primary->append_conf('postgresql.conf', 'full_page_writes=off');
$node_primary->append_conf('postgresql.conf', 'log_min_messages=debug2');

$node_primary->start;

my $psql_h = $node_primary->background_psql($regress_db);
# begin an transaction
$psql_h->query_safe(q(begin;));

# create one table and insert some data
$psql_h->query_safe(
	q(
create table test (id int, name char(128) default 'A');
insert into test select generate_series(1,64);
));

# do a successful commit transaction to flush all the above wal records
my $timeout = 60;
$node_primary->safe_psql(
	$regress_db,
	"checkpoint;",
	timeout => $timeout);

# sigstop checkpoint: No more checkpoint
kill_process($node_primary->{_pid}, 'checkpointer');

# insert more data
$psql_h->query_safe(q(insert into test values (65);));

# do a successful commit transaction to flush all the above wal records
$node_primary->safe_psql(
	$regress_db,
	"create table temp_test (id int);",
	timeout => $timeout);

# sigstop walwriter: do not flush the following abort record
kill_process($node_primary->{_pid}, 'walwriter');

# get the file path of test
my $file_path = $psql_h->query_safe(q(select pg_relation_filepath('test');));
$file_path =~ s/^\s+|\s+$//g;
print "rel file path is $file_path\n";

# abort the above transaction
$psql_h->query_safe(q(abort;));

# close connection
$psql_h->quit;

# immediate shutdown primary: do not flush the above abort record
$node_primary->stop('immediate');

my $log_location = -s $node_primary->logfile;
# start
$node_primary->start;

$node_primary->log_check('find invalid page for rel file path', $log_location,
						 log_like => [qr/page [0-9]+ of relation $file_path does not exist/]);

$node_primary->log_check('find dropped invalid page for rel file path', $log_location,
						 log_like => [qr/page [0-9]+ of relation $file_path has been dropped/]);

$node_primary->stop();
