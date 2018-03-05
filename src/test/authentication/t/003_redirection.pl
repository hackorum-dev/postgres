# Test redirection authentication method.
#
# This test cannot run on Windows as Postgres cannot be set up with Unix
# sockets and needs to go through SSPI.

use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More;
if ($windows_os)
{
	plan skip_all => "authentication tests cannot run on Windows";
}
else
{
	plan tests => 3;
}

# Delete pg_hba.conf from the given node, add a new entry to it
# and then execute a reload to refresh it.
sub reset_pg_hba
{
	my $node       = shift;
	my $hba_method = shift;
	my $redirect_endpoint = shift;

	unlink($node->data_dir . '/pg_hba.conf');
	$node->append_conf('pg_hba.conf', "local all all $hba_method $redirect_endpoint");
	$node->reload;
}

# Test access for a single role, useful to wrap all tests into one.
sub test_login
{
	my $node          = shift;
	my $role          = shift;
	my $password      = shift;
	my $expected_res  = shift;
	my $status_string = 'failed';

	$status_string = 'success' if ($expected_res eq 0);

	$ENV{"PGPASSWORD"} = $password;
	my $res = $node->psql('postgres', undef, extra_params => [ '-U', $role ]);
	is($res, $expected_res,
		"authentication $status_string for role $role with password $password"
	);
}

# Initialize node1.
my $node1 = get_new_node('master');
$node1->init;
$node1->start;
$node1->safe_psql(
	'postgres',
	"CREATE ROLE pguser1 LOGIN PASSWORD 'postgres';
");

# Initialize node2, the redirect target.
my $node2 = get_new_node('redirect');
$node2->init;
$node2->start;
$node2->safe_psql(
	'postgres',
	"CREATE ROLE pguser2 LOGIN PASSWORD 'postgres';
");

# Host is identical as both nodes reside on the same machine
my $host = $node1->host;

my $node1_port = $node1->port;
# 1. Test a redirected connection from node1 to itself.
# Add the redirect authentication method to the node1's pg_hba.conf to set up redirection to itself.
reset_pg_hba($node1, 'redirect', "$host,$node1_port");
# A redirect from a node to itself should fail after PGREDIRECTLIMIT (default is 5) retries.
test_login($node1, 'pguser1', "postgres",   2);


my $node2_port = $node2->port;
# 2. Test a redirected connection from node1 to node2 with correct creds.
# Add the redirect authentication method to the node1's pg_hba.conf to set up redirection to node2.
reset_pg_hba($node1, 'redirect', "$host,$node2_port");
# A redirect from a node to another should succeed, given the correct creds are used.
test_login($node1, 'pguser2', "postgres",   0);


# 3. Test a redirected connection from node1 to node2 with wrong creds.
# Add the redirect authentication method to the node1's pg_hba.conf to set up redirection to node2.
reset_pg_hba($node1, 'redirect', "$host,$node2_port");
# A redirect from a node to another should fail if the wrong creds are used.
test_login($node1, 'pguser', "postgres",   2);
