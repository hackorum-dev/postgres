# target_server_type
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 10000;
use Scalar::Util qw(blessed);

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1);
$node_master->append_conf('postgresql.conf', "listen_addresses = '$PostgresNode::test_localhost'");
$node_master->start;

# Take backup
my $backup_name = 'my_backup';
$node_master->backup($backup_name);

# Create streaming standby 1 linking to master
my $node_standby_1 = get_new_node('standby_1');
$node_standby_1->init_from_backup($node_master, $backup_name, has_streaming => 1);
$node_standby_1->append_conf('postgresql.conf', "listen_addresses = '$PostgresNode::test_localhost'");
$node_standby_1->start;

# Create streaming standby 2 linking to master
my $node_standby_2 = get_new_node('standby_2');
$node_standby_2->init_from_backup($node_master, $backup_name, has_streaming => 1);
$node_standby_2->append_conf('postgresql.conf', "listen_addresses = '$PostgresNode::test_localhost'");
$node_standby_2->start;

sub get_host_port
{
	my $node = shift;
	return "$PostgresNode::test_localhost:" . $node->port;
}

sub multiconnstring
{
	my $nodes    = shift;
	my $database = shift || "postgres";
	my $params   = shift;
	my $extra    = "";
	if ($params)
	{
		my @cs;
		while (my ($key, $val) = each %$params)
		{
			push @cs, $key . "=" . $val;
		}
		$extra = "?" . join("&", @cs);
	}
	my $str =
		"postgresql://"
		. join(",", map({ get_host_port($_) } @$nodes))
		. "/$database$extra";

	return $str;
}

#
# Copied from PosgresNode.pm passing explicit connect-string instead of
# constructed from object
#

sub psql2
{
	# We expect dbname to be part of connstr
	my ($connstr, $sql, %params) = @_;

	my $stdout            = $params{stdout};
	my $stderr            = $params{stderr};
	my @psql_params       = ('psql', '-XAtq', '-d', $connstr, '-f', '-');

	# Allocate temporary ones to capture them so we
	# can return them. Otherwise we won't redirect them at all.
	if (!defined($stdout))
	{
		my $temp_stdout = "";
		$stdout = \$temp_stdout;
	}
	if (!defined($stderr))
	{
		my $temp_stderr = "";
		$stderr = \$temp_stderr;
	}

	# IPC::Run would otherwise append to existing contents:
	$$stdout = "" if ref($stdout);
	$$stderr = "" if ref($stderr);

	my $ret;

	# Run psql and capture any possible exceptions.  If the exception is
	# because of a timeout and the caller requested to handle that, just return
	# and set the flag.  Otherwise, and for any other exception, rethrow.
	#
	# For background, see
	# http://search.cpan.org/~ether/Try-Tiny-0.24/lib/Try/Tiny.pm
	do
	{
		local $@;
		eval {
			my @ipcrun_opts = (\@psql_params, '<', \$sql);
			push @ipcrun_opts, '>',  $stdout if defined $stdout;
			push @ipcrun_opts, '2>', $stderr if defined $stderr;

			IPC::Run::run @ipcrun_opts;
			$ret = $?;
		};
	};

	return ($ret, $$stdout, $$stderr);
}

# returns host:port retrieved via \conninfo
sub psql_conninfo
{
	my ($connstr) = shift;
	my ($retcode, $stdout, $stderr) = psql2($connstr, '\conninfo');
	if ($retcode == 0 && $stdout =~ /on host "([^"]*)" at port "([^"]*)"/s)
	{
		return "$1:$2";
	}
	else
	{
		return "STDOUT:$stdout\nSTDERR:$stderr";
	}
}

my $conninfo;
my $expected;
my $connnstr;

# Test 10 - one of standbys is not available
$node_standby_1->stop();
# Test 10.1
for my $i (1 .. 10000)
{
	$conninfo =
		psql_conninfo(
			multiconnstring([ $node_standby_1, $node_master, $node_standby_2 ]));
	is($conninfo, get_host_port($node_master), "first node is unavailable");
}
