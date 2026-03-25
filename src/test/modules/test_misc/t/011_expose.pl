# Copyright (c) 2026, PostgreSQL Global Development Group

# Test gathering information before authentication via expose_* variables

# Force use of TCP/IP - must be called before the 'use' section
INIT{ $PostgreSQL::Test::Utils::use_unix_sockets = 0; }

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node1');

# Set as logical here so we can restart it as a replica later
$node->init(allows_streaming => 'logical');
$node->start;

my $server_version = $node->safe_psql('postgres', 'show server_version_num');
my $bindir = $node->config_data('--bindir');
my $datadir = $node->data_dir;
my $cdata = qx{$bindir/pg_controldata -D $datadir 2>&1};
my ($sysid) = $cdata =~ /Database system identifier:\s+(\d+)/;
my $receive_length = 200;

my ($socket, $response, $test);

sub call_socket {
	my $string = shift;
	$socket->close() if defined $socket;
	$socket = $node->raw_connect();
	$socket->send($string);
	$response = '';
	select(undef, undef, undef, 0.1);
	$socket->recv($response, $receive_length);
	return;
}

$test = 'GET /info returns nothing when nothing is listening';
call_socket('GET /info');
is ($response, '', $test);

$test = 'HEAD /replica returns nothing when nothing is listening';
call_socket('HEAD /replica');
is ($response, '', $test);

$node->append_conf('postgresql.conf', "expose_information = 'replica'");
$node->reload();

$test = q{GET /replica returns HTTP code 200 when expose_information contains 'replica' (primary)};
call_socket('GET /replica');
like ($response, qr{^HTTP/1.1 200 }, $test);

$test = q{GET /replica returns "0" when expose_information contains 'replica' (primary)};
like ($response, qr{\r\n0\r\n}, $test);

$test = q{HEAD /replica returns HTTP code 503 when expose_information contains 'replica' (primary)};
call_socket('HEAD /replica');
like ($response, qr{^HTTP/1.1 503 }, $test);

$test = q{GET /info returns "REPLICA: 0" when expose_information contains 'replica' (primary)};
call_socket('GET /info');
like ($response, qr{REPLICA: 0\r\n}, $test);

$test = q{GET /info does not return version information when expose_information does not contain 'version'};
unlike ($response, qr{VERSION}, $test);

$test = q{GET /info does not return sysid information when expose_information does not contain 'sysid'};
unlike ($response, qr{SYSID}, $test);

$node->append_conf('postgresql.conf', "expose_information= 'replica,sysid,version'");
$node->reload();

$test = q{GET /info returns correct version when expose_information contains 'version'};
call_socket('GET /info');
like ($response, qr/VERSION: $server_version/, $test);

$test = q{GET /info returns correct value when expose_information contains 'sysid'};
like ($response, qr/SYSID: $sysid/, $test);

$test = q{Get /sysid returns correct value when expose_information contains 'sysid'};
call_socket('Get /sysid'); ## Not required to be all uppercase according to the spec!
like ($response, qr/^$sysid\r\n/m, $test);

$test = q{GET /version returns correct value when expose_information contains 'version'};
call_socket('GET /version');
like ($response, qr/^$server_version\r\n/m, $test);

$test = 'GET /foobar returns nothing';
call_socket('GET /foobar');
is ($response, '', $test);

$node->set_standby_mode();
$node->restart();

$test = q{GET /replica returns HTTP code 200 when expose_information contains 'replica' (replica)};
call_socket('GET /replica');
like ($response, qr{^HTTP/1.1 200 }, $test);

$test = q{GET /replica returns "1" when expose_information contains 'replica' (replica)};
like ($response, qr{^1\r\n}m, $test);

$test = q{HEAD /replica returns HTTP code 200 when expose_information contains 'replica' (replica)};
call_socket('HEAD /replica');
like ($response, qr{^HTTP/1.1 200 }, $test);

$test = q{GET /info returns "REPLICA: 1" when expose_information contains 'replica' (replica)};
call_socket('GET /info');
like ($response, qr/REPLICA: 1/, $test);

$node->append_conf('postgresql.conf', "expose_information=''");
$node->reload();

$test = q{GET /version returns nothing after expose_information no longer has 'version'};
call_socket('GET /version');
is ($response, '', $test);

$socket->close();

done_testing();
