# test for promote with trigger_file
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 3;
use File::Copy;

my $triggers_dir = TestLib::tempdir('tmp_trigger_file');

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(
	has_archiving    => 1,
	allows_streaming => 1);
my $backup_name = 'my_backup';

# Start it
$node_master->start;

# Take backup for standby
$node_master->backup($backup_name);

# Initialize standby nodes from backup
# standby 1 check startup parameter
my $node_standby_1 = get_new_node('standby_1');
$node_standby_1->init_from_backup($node_master, $backup_name, has_restoring => 1);
my $standby_1_trigger_file = $triggers_dir . '/trigger_1';
$node_standby_1->append_conf('recovery.conf',
	"trigger_file = '$standby_1_trigger_file'");
$node_standby_1->start;

# standby 2 will be reload
my $node_standby_2 = get_new_node('standby_2');
$node_standby_2->init_from_backup($node_master, $backup_name, has_restoring => 1);
my $standby_2_trigger_file = $triggers_dir . '/trigger_2';
$node_standby_2->start;

sub check_in_recovery
{
	my ($node) = @_;
	my $result = $node->safe_psql('postgres', "SELECT pg_is_in_recovery()::int");
	is($result, '1', $node->name . ' in recovery');
}

check_in_recovery($node_standby_1);
check_in_recovery($node_standby_2);

TestLib::append_to_file($standby_1_trigger_file, '');
TestLib::append_to_file($standby_2_trigger_file, '');

$node_standby_1->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
	or die "Timed out while waiting for standby 1 to promote";

check_in_recovery($node_standby_2); # be sure still in recovery
$node_standby_2->append_conf('recovery.conf',
	"trigger_file = '$standby_2_trigger_file'");
$node_standby_2->reload;

$node_standby_2->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
	or die "Timed out while waiting for standby 2 to promote";;
