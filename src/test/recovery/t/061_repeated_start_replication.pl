
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that START_REPLICATION can be issued again on a walsender connection
# after an earlier stream on it ended with CopyDone.  psql does not support
# COPY BOTH mode, so the test speaks the protocol over a raw socket.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IO::Select;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 'logical');

# The client below only does cleartext password authentication, so put a
# password rule for the test role ahead of the default rules (which use SSPI
# on Windows).
my $hba_file = $node->data_dir . '/pg_hba.conf';
my $hba = slurp_file($hba_file);
open my $hba_fh, '>', $hba_file or die "could not open \"$hba_file\": $!";
if ($PostgreSQL::Test::Utils::use_unix_sockets)
{
	print $hba_fh "local all regress_walsender password\n";
}
else
{
	print $hba_fh "host all regress_walsender "
	  . $node->host
	  . "/32 password\n";
}
print $hba_fh $hba;
close $hba_fh;

$node->start;

if (!$node->raw_connect_works())
{
	plan skip_all => "this test requires working raw_connect()";
}

$node->safe_psql('postgres',
	"CREATE ROLE regress_walsender LOGIN REPLICATION PASSWORD 'walsender'");
$node->safe_psql('postgres', "CREATE TABLE t (v text)");
$node->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('test_slot', 'test_decoding')"
);

my $sock = $node->raw_connect();
my $select = IO::Select->new($sock);

# Read exactly $len bytes from the server.
sub read_bytes
{
	my ($len) = @_;
	my $buf = '';

	while (length($buf) < $len)
	{
		$select->can_read($PostgreSQL::Test::Utils::timeout_default)
		  or die "timed out waiting for the server";
		my $chunk;
		my $n = sysread($sock, $chunk, $len - length($buf));
		die "connection closed by the server" if !$n;
		$buf .= $chunk;
	}
	return $buf;
}

# Read one message, returning its type and contents.  An ErrorResponse
# fails the test.
sub read_message
{
	my ($type, $len) = unpack('aN', read_bytes(5));
	my $body = read_bytes($len - 4);

	if ($type eq 'E')
	{
		my %fields = map { substr($_, 0, 1) => substr($_, 1) }
		  split(/\0/, $body);
		die "server error: $fields{M}";
	}
	return ($type, $body);
}

sub send_message
{
	my ($type, $body) = @_;

	$sock->send($type . pack('N', length($body) + 4) . $body);
}

# Read messages up to and including ReadyForQuery, returning their types.
sub read_until_ready
{
	my @types;

	while (1)
	{
		my ($type) = read_message();
		push @types, $type;
		return @types if $type eq 'Z';
	}
}

# Run a START_REPLICATION command.  For logical replication, read the stream
# until a CopyData message matches $wanted.  Then end the stream with
# CopyDone.  Returns "streamed" if the server answered with its own CopyDone,
# or how the stream ended otherwise.
sub stream
{
	my ($command, $wanted) = @_;

	send_message('Q', "$command\0");
	my ($type, $body) = read_message();
	return "got '$type' instead of CopyBothResponse" if $type ne 'W';

	while (defined $wanted)
	{
		($type, $body) = read_message();
		if ($type ne 'd')
		{
			read_until_ready() if $type ne 'Z';
			return "stream ended with '$type' before the data arrived";
		}
		last if $body =~ $wanted;
	}

	send_message('c', '');
	my @types = read_until_ready();
	return (grep { $_ eq 'c' } @types)
	  ? "streamed"
	  : "no CopyDone from the server";
}

# Start up and authenticate.
my $startup = pack('nn', 3, 0)
  . "user\0regress_walsender\0database\0postgres\0replication\0database\0\0";
$sock->send(pack('N', length($startup) + 4) . $startup);
while (1)
{
	my ($type, $body) = read_message();
	last if $type eq 'Z';
	next if $type ne 'R';

	my $code = unpack('N', $body);
	if ($code == 3)
	{
		send_message('p', "walsender\0");
	}
	elsif ($code != 0)
	{
		die "unexpected authentication request $code";
	}
}

$node->safe_psql('postgres', "INSERT INTO t VALUES ('first')");
is(stream('START_REPLICATION SLOT test_slot LOGICAL 0/0', qr/'first'/),
	'streamed', 'logical replication');

$node->safe_psql('postgres', "INSERT INTO t VALUES ('second')");
is(stream('START_REPLICATION SLOT test_slot LOGICAL 0/0', qr/'second'/),
	'streamed', 'logical replication again on the same connection');

is(stream('START_REPLICATION PHYSICAL ' . $node->lsn('flush'), undef),
	'streamed', 'physical replication on the same connection');

$node->safe_psql('postgres', "INSERT INTO t VALUES ('third')");
is( stream('START_REPLICATION SLOT test_slot LOGICAL 0/0', qr/'third'/),
	'streamed',
	'logical replication after physical replication on the same connection');

send_message('X', '');
$sock->close();
$node->stop;

done_testing();
