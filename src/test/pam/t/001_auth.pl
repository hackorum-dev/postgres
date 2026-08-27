
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use FindBin;
use lib "$FindBin::RealBin/..";

use File::Copy;
use File::Basename;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

if ($ENV{with_pam} ne 'yes')
{
	plan skip_all => 'PAM not supported by this build';
}
elsif (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bpam\b/)
{
	plan skip_all =>
	  'Potentially unsafe test PAM not enabled in PG_TEST_EXTRA';
}
elsif (!check_pg_config("#define HAVE_PAM_START_CONFDIR 1"))
{
	plan skip_all =>
	  'PAM tests requires Postgres build with pam library that supports pam_start_confdir function';
}

note "setting up PostgreSQL instance";

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf('postgresql.conf', "log_connections = all\n");
# Needed to allow connect_fails to inspect postmaster log:
$node->append_conf('postgresql.conf', "log_min_messages = debug2");
$node->start;

$node->safe_psql('postgres', 'CREATE USER test0;');
$node->safe_psql('postgres', 'CREATE USER test1;');
$node->safe_psql('postgres', 'CREATE USER test2;');
$node->safe_psql('postgres', 'CREATE USER test3;');
$node->safe_psql('postgres', 'CREATE USER test4;');

note "running tests";

my $test_temp_= PostgreSQL::Test::Utils::tempdir("pam-001_auth");

my $test_temp = PostgreSQL::Test::Utils::tempdir("pam-001_auth");
my $test_pam_conf_pg = "$test_temp/postgresql";
my $test_pam_conf_pg2 = "$test_temp/postgresql2";
my $test_pam_conf_pg3 = "$test_temp/postgresql3";

my $test_pam_exec_dir = PostgreSQL::Test::Utils::tempdir("001_authpam_exec");
my $test_pam_exec = "$test_pam_exec_dir/user.sh";
my $test_pam_exec_log = "$test_pam_exec_dir/log";

append_to_file($test_pam_exec, qq{#! /bin/sh

set -x

read pam_passwd

if [ "\$PAM_USER" != "test3" ]; then
    exit 1
fi
if [ "\$pam_passwd" != "password3" ]; then
    exit 2
fi
exit 0
});

append_to_file($test_pam_conf_pg, qq{
auth   required pam_permit.so
account  required pam_permit.so
});

append_to_file($test_pam_conf_pg2, qq{
auth   required pam_deny.so
account  required pam_deny.so
});

append_to_file($test_pam_conf_pg3, qq{
auth   required pam_exec.so debug expose_authtok log=$test_pam_exec_log /bin/sh $test_pam_exec
account  required pam_permit.so
});

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf(
	'pg_hba.conf',
	qq{
local all test0 pam pamconfdir=$test_temp
local all test1 pam pamconfdir=$test_temp pamservice=postgresql
local all test2 pam pamconfdir=$test_temp pamservice=postgresql2
local all test3 pam pamconfdir=$test_temp pamservice=postgresql3
local all test4 pam pamconfdir=$test_temp pamservice=postgresql3
});
$node->restart;

$node->connect_ok("user=test0",
	"correctly routes pam.d/postgresql by default, connection succeds due to pam_accept.so");

$node->connect_ok("user=test1",
	"correctly routes pam.d/postgresql by explicit, connection succeds due to pam_accept.so");

$node->connect_fails("user=test2",
	"correctly routes to pam.d/postgresql2, connection fails due to pam_deny.sio");

$node->connect_ok("user=test3 password=password3",
	"correctly routes pam.d/postgresql3, accepted");

$node->connect_fails("user=test3 password=wrongpassword3",
	"correctly routes to pam.d/postgresql3, rejected due to wrong password");

$node->connect_fails("user=test4",
	"correctly routes to pam.d/postgresql3, rejected due to wrong user");

$node->teardown_node;

done_testing();
