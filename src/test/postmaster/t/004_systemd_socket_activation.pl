
# Copyright (c) 2024-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use locale;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Time::HiRes qw(sleep);
use Test::More;

if ($ENV{with_systemd} ne 'yes')
{
	plan skip_all => 'systemd not supported by this build';
}

# Node initialization
my $node = PostgreSQL::Test::Cluster->new('socket');

$node->init();

my $port = $node->port;
my $pgdata = $node->data_dir;
my $logfile = $node->logfile;
my $ret;
my $logstart;

# Starting postgres using systemd-socket-activate
# -l 127.0.0.1:5432 -l 192.168.100.49:5432
# -l [2600:1700:31f0:6b7f:5a47:caff:fe73:924f]:5432
# -l /tmp/.s.PGSQL.5432
# -l /run/user/1000/.s.PGSQL.5432 $PG_DEV_INST/bin/postgres
# -D $PGDATA
# Start postgresql using system
my $tcp_fd = " -l 127.0.0.1:".$port;
my $unix_fd = " -l /tmp/.s.PGSQL.".$port;
my $pg_cmd = " postgres -D ".$pgdata." &>>".$logfile." &";
my $systemd_cmd = "systemd-socket-activate ".$tcp_fd.$unix_fd.$pg_cmd;
my $systemd_bad1 = "systemd-socket-activate ".$unix_fd.$tcp_fd.$pg_cmd;
my $systemd_bad2 = "systemd-socket-activate ".$tcp_fd.$pg_cmd;

# Setup listen_address
$node->append_conf('postgresql.conf', "listen_addresses = '127.0.0.1'");
# Setup unix_socket_directory
$node->append_conf('postgresql.conf', "unix_socket_directories = '/tmp'");

# Start with an empty logfile
$ret = PostgreSQL::Test::Utils::system_log( "echo ''>".$logfile );

note 'Bad test 1';
# Test passing FD in wrong order
$ret = PostgreSQL::Test::Utils::system_log( $systemd_bad1 );
ok($ret == "0", "systemd-socket-activate started");
$logstart = -s $node->logfile;
# psql: error: connection to server at "127.0.0.1", port 5432 failed: FATAL:  the database system is starting up
$node->connect_fails("host=127.0.0.1", "Check for failure due to wrong FD order",
  expected_stderr => qr/failed: server closed the connection unexpectedly/);
ok( $node->log_contains(qr/FATAL:  Sockets \(0\) passed by systemd is not same type as configured in postgresql.conf/, $logstart),
	"Check for socket 0 mismtach message");


note 'Bad test 2';
# Only pass TCP FD
$ret = PostgreSQL::Test::Utils::system_log( $systemd_bad2 );
ok($ret == "0", "systemd-socket-activate started");
$logstart = -s $node->logfile;
# psql: error: connection to server at "127.0.0.1", port 5432 failed: FATAL:  the database system is starting up
$node->connect_fails("host=127.0.0.1", "Check for failure due to wrong FD passed",
  expected_stderr => qr/failed: server closed the connection unexpectedly/);
ok( $node->log_contains(qr/FATAL:  Sockets passed by systemd  \(1\) are less than listen_addresses configured in postgresql.conf/, $logstart),
	"Check for socket 0 mismtach message");

note 'Good test';
# This is a good test with all the correct settings
$ret = PostgreSQL::Test::Utils::system_log( $systemd_cmd );
ok($ret == "0", "systemd-socket-activate started");
# psql: error: connection to server at "127.0.0.1", port 5432 failed: FATAL:  the database system is starting up
$node->connect_fails("host=127.0.0.1", "database system is starting up", expected_stderr => qr/FATAL:  the database system is starting up/);
sleep(1);
$node->connect_ok("host=127.0.0.1", "Connect to IP works");
$node->connect_ok("host=/tmp", "Connect to unix_sock works");
$node->stop();
done_testing();
