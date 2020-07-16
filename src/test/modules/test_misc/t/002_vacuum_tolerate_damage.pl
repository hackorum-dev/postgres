# Verify the vacuum_tolerate_damage GUC

use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 2;

# Initialize a test cluster
my $node = get_new_node('primary');
$node->init();
$node->start;

# Run a SQL command and return psql's stderr
sub run_sql_command
{
	my $sql = shift;
	my $stderr;

	$node->psql(
		'postgres',
		$sql,
		stderr        => \$stderr);
	return $stderr;
}

my $output;

# Insert 2 tuple in the table and update the relfrozenxid for the table to
# the future xid.
run_sql_command(
	"create table test_vac (a int) WITH (autovacuum_enabled = off);
	 insert into test_vac values (1), (2);
	 update pg_class set relfrozenxid=txid_current()::text::xid where relname='test_vac';");

$output = run_sql_command('vacuum(freeze, disable_page_skipping) test_vac;');
ok( $output =~ m/ERROR:.*found xmin.*before relfrozenxid/);

# set the vacuum_tolerate_damage and run again
$output = run_sql_command('set vacuum_tolerate_damage=true;
						   vacuum(freeze, disable_page_skipping) test_vac;');


# this time we should get WARNING for both the tuples
ok( scalar( @{[ $output=~/WARNING:.*found xmin.*before relfrozenxid/gi ]}) == 2);

run_sql_command('DROP TABLE test_vac;');

$node->stop('fast');
