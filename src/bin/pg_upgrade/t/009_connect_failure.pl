# Copyright (c) 2026, PostgreSQL Global Development Group

# Failed nonblocking connects must wake pg_upgrade's Windows task loop.
use strict;
use warnings FATAL => 'all';

use IPC::Run;
use Errno qw(ECONNREFUSED);
use Socket
  qw(AF_UNSPEC AF_INET6 SOCK_STREAM NI_NUMERICHOST getaddrinfo getnameinfo);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Windows select() connection-failure behavior'
  unless $windows_os;

# This optional module is not bundled with every supported Perl version.
eval { require IO::Socket::IP; 1; }
  or plan skip_all => 'IO::Socket::IP is required for the IPv6 precondition';

# This trigger needs localhost to try IPv6 before the IPv4-only server.
# Skip explicitly when the resolver cannot supply that ordering.
my ($lookup_error, @addresses) = getaddrinfo('localhost', '5432',
	{ family => AF_UNSPEC, socktype => SOCK_STREAM });
plan skip_all => 'localhost address lookup failed' if $lookup_error;
my @hosts;
for my $address (@addresses)
{
	my ($name_error, $host) = getnameinfo($address->{addr}, NI_NUMERICHOST);
	plan skip_all => 'numeric localhost lookup failed' if $name_error;
	push @hosts, $host;
}
plan skip_all => 'localhost must resolve to ::1 first and include 127.0.0.1'
  unless @hosts && $hosts[0] eq '::1' && grep { $_ eq '127.0.0.1' } @hosts;

my $oldnode = PostgreSQL::Test::Cluster->new('old_node');
my $newnode = PostgreSQL::Test::Cluster->new('new_node');
$oldnode->init(extra => ['--locale=C']);
$newnode->init(extra => ['--locale=C']);
$oldnode->append_conf('postgresql.conf', "listen_addresses = '127.0.0.1'");
$newnode->append_conf('postgresql.conf', "listen_addresses = '127.0.0.1'");
$oldnode->start;

# Confirm the initial endpoint refuses connections on this owned port.
my $ipv6_connection = IO::Socket::IP->new(
	PeerHost => '::1',
	PeerPort => $oldnode->port,
	Family => AF_INET6,
	Type => SOCK_STREAM,
	Timeout => 3);
my $connection_refused = $!{ECONNREFUSED};
my $connection_error = "$!";
if ($ipv6_connection)
{
	close $ipv6_connection;
	$oldnode->stop;
	plan skip_all => 'the IPv6 endpoint unexpectedly accepts connections';
}
elsif (!$connection_refused)
{
	$oldnode->stop;
	plan skip_all =>
	  "IPv6 endpoint did not refuse the connection: $connection_error";
}

# pg_upgrade writes its logs relative to its working directory.
chdir $PostgreSQL::Test::Utils::tmp_check;

my @command = (
	'pg_upgrade', '--check', '--no-sync', '--jobs=2',
	'--old-datadir' => $oldnode->data_dir,
	'--new-datadir' => $newnode->data_dir,
	'--old-bindir' => $oldnode->config_data('--bindir'),
	'--new-bindir' => $newnode->config_data('--bindir'),
	'--old-port' => $oldnode->port,
	'--new-port' => $newnode->port);
my @checks;
for my $host ('127.0.0.1', 'localhost')
{
	my ($stdout, $stderr) = ('', '');
	my ($harness, $error, $status);
	{
		# IPv4 is the successful-first control; localhost must fall back.
		local $ENV{PGHOST} = $host;
		local $ENV{PGHOSTADDR};
		delete $ENV{PGHOSTADDR};
		my $timer =
		  IPC::Run::timeout($PostgreSQL::Test::Utils::timeout_default);
		eval {
			$harness = IPC::Run::start(
				\@command,
				'>' => \$stdout,
				'2>' => \$stderr,
				$timer);
			$harness->finish;
			$status = $?;
		};
		$error = $@;
		$harness->kill_kill if $error && defined $harness;
	}
	push @checks, [ $host, $stdout, $stderr, $error, $status ];
	last if $error || !defined $status || $status;
}

# Cleanup precedes assertions and includes a server left after a timeout.
$newnode->_update_pid(-1);
$newnode->stop;
$oldnode->stop;

for my $check (@checks)
{
	my ($host, $stdout, $stderr, $error, $status) = @$check;
	ok( !$error && defined $status && $status == 0,
		"pg_upgrade --check completes with host=$host");
	like(
		$stdout,
		qr/Clusters are compatible/,
		"pg_upgrade checks both clusters with host=$host");
	diag("$error\n$stdout\n$stderr") if $error || !defined $status || $status;
}
is(scalar @checks, 2, 'both successful-first and refused-first cases ran');
ok(!-e $oldnode->data_dir . '/postmaster.pid', 'old server is stopped');
ok(!-e $newnode->data_dir . '/postmaster.pid', 'new server is stopped');
done_testing();
