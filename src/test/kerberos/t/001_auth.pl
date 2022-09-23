
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Sets up a KDC and then runs a variety of tests to make sure that the
# GSSAPI/Kerberos authentication and encryption are working properly,
# that the options in pg_hba.conf and pg_ident.conf are handled correctly,
# that the server-side pg_stat_gssapi view reports what we expect to
# see for each test and that SYSTEM_USER returns what we expect to see.
#
# Since this requires setting up a full KDC, it doesn't make much sense
# to have multiple test scripts (since they'd have to also create their
# own KDC and that could cause race conditions or other problems)- so
# just add whatever other tests are needed to here.
#
# See the README for additional information.

use strict;
use warnings;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;
use Time::HiRes qw(usleep);

if ($ENV{with_gssapi} ne 'yes')
{
	plan skip_all => 'GSSAPI/Kerberos not supported by this build';
}
elsif ($ENV{PG_TEST_EXTRA} !~ /\bkerberos\b/)
{
	plan skip_all => 'Potentially unsafe test GSSAPI/Kerberos not enabled in PG_TEST_EXTRA';
}

my $krb5_config  = 'krb5-config';
my $kinit        = 'kinit';
my $kdb5_util    = 'kdb5_util';
my $kadmin_local = 'kadmin.local';
my $krb5kdc      = 'krb5kdc';

# control variable for checking if all executables are found
my $is_krb5_found = 0;

# Given $krb5_path_prefix (possible paths of kerberos executables),
# first check value of $is_krb5_found:
# if it is truthy value this means executables are already found so
# don't try to locate further, else add '/bin/' and '/sbin/'
# to the $krb5_path_prefix and try to locate all required kerberos executables
# ($krb5_config, $kinit, $kdb5_util, $kadmin_local, $krb5kdc).
# If all executables are found, set $is_krb5_found to 1 and use executables
# from this path; even if one of the executables is not found, skip this path.
# If all paths are searched and executables are still not found,
# test will try to use kerberos executables from $PATH.
sub test_krb5_paths
{
	my ($krb5_path_prefix) = (@_);

	# if executables are already found, return
	if ($is_krb5_found)
	{
		return;
	}

	my ($kdb5_util_path, $kadmin_local_path, $krb5kdc_path, $krb5_config_path, $kinit_path, @krb5_file_paths, $bin, $sbin);

	# remove '\n' since 'krb5-config --prefix' returns path ends with '\n'
	chomp($krb5_path_prefix);
	# remove trailing slashes
	$krb5_path_prefix =~ s{(.+)/\z}{$1};
	$bin = '/bin/';
	$sbin = '/sbin/';
	$kdb5_util_path    = $krb5_path_prefix . $sbin . $kdb5_util;
	$kadmin_local_path = $krb5_path_prefix . $sbin . $kadmin_local;
	$krb5kdc_path      = $krb5_path_prefix . $sbin . $krb5kdc;
	$krb5_config_path  = $krb5_path_prefix . $bin . $krb5_config;
	$kinit_path        = $krb5_path_prefix . $bin . $kinit;
	@krb5_file_paths = ($kdb5_util_path, $kadmin_local_path, $krb5kdc_path, $krb5_config_path, $kinit_path);

	# check if all of the executables exist, even if one of the executables
	# is not found; return
	foreach (@krb5_file_paths)
	{
		if(! -e $_)
		{
			return;
		}
	}
	# all executables are found
	$krb5_config  = $krb5_config_path;
	$kinit        = $kinit_path;
	$kdb5_util    = $kdb5_util_path;
	$kadmin_local = $kadmin_local_path;
	$krb5kdc      = $krb5kdc_path;

	# set $is_krb5_found to the 1 to not check other possible paths
	$is_krb5_found = 1;
}

# first try to use 'krb5-config --prefix', then try hardcoded paths.
# if executables are still not found after trying these paths,
# try to use them from $PATH.
my $krb5_config_prefix_cmd = $krb5_config . ' --prefix';
test_krb5_paths(`$krb5_config_prefix_cmd`);
if (! $is_krb5_found)
{
	if ($^O eq 'darwin')
	{
		# krb5 is placed at /usr/local/opt for Intel CPU darwin
		test_krb5_paths('/usr/local/opt/krb5/');
		# krb5 is placed at /opt/homebrew/opt/ for arm CPU darwin
		test_krb5_paths('/opt/homebrew/opt/krb5/');
		# krb5 is placed at /opt/local/ for MacPorts
		test_krb5_paths('/opt/local/');
	}
	elsif ($^O eq 'freebsd')
	{
		test_krb5_paths('/usr/local/');
	}
	elsif ($^O eq 'linux')
	{
		test_krb5_paths('/usr/');
	}
}

my $host     = 'auth-test-localhost.postgresql.example.com';
my $hostaddr = '127.0.0.1';
my $realm    = 'EXAMPLE.COM';

my $krb5_conf   = "${PostgreSQL::Test::Utils::tmp_check}/krb5.conf";
my $kdc_conf    = "${PostgreSQL::Test::Utils::tmp_check}/kdc.conf";
my $krb5_cache  = "${PostgreSQL::Test::Utils::tmp_check}/krb5cc";
my $krb5_log    = "${PostgreSQL::Test::Utils::log_path}/krb5libs.log";
my $kdc_log     = "${PostgreSQL::Test::Utils::log_path}/krb5kdc.log";
my $kdc_port    = PostgreSQL::Test::Cluster::get_free_port();
my $kdc_datadir = "${PostgreSQL::Test::Utils::tmp_check}/krb5kdc";
my $kdc_pidfile = "${PostgreSQL::Test::Utils::tmp_check}/krb5kdc.pid";
my $keytab      = "${PostgreSQL::Test::Utils::tmp_check}/krb5.keytab";

my $dbname      = 'postgres';
my $username    = 'test1';
my $application = '001_auth.pl';

note "setting up Kerberos";

my ($stdout, $krb5_version);
run_log [ $krb5_config, '--version' ], '>', \$stdout
  or BAIL_OUT("could not execute krb5-config");
BAIL_OUT("Heimdal is not supported") if $stdout =~ m/heimdal/;
$stdout =~ m/Kerberos 5 release ([0-9]+\.[0-9]+)/
  or BAIL_OUT("could not get Kerberos version");
$krb5_version = $1;

append_to_file(
	$krb5_conf,
	qq![logging]
default = FILE:$krb5_log
kdc = FILE:$kdc_log

[libdefaults]
default_realm = $realm

[realms]
$realm = {
    kdc = $hostaddr:$kdc_port
}!);

append_to_file(
	$kdc_conf,
	qq![kdcdefaults]
!);

# For new-enough versions of krb5, use the _listen settings rather
# than the _ports settings so that we can bind to localhost only.
if ($krb5_version >= 1.15)
{
	append_to_file(
		$kdc_conf,
		qq!kdc_listen = $hostaddr:$kdc_port
kdc_tcp_listen = $hostaddr:$kdc_port
!);
}
else
{
	append_to_file(
		$kdc_conf,
		qq!kdc_ports = $kdc_port
kdc_tcp_ports = $kdc_port
!);
}
append_to_file(
	$kdc_conf,
	qq!
[realms]
$realm = {
    database_name = $kdc_datadir/principal
    admin_keytab = FILE:$kdc_datadir/kadm5.keytab
    acl_file = $kdc_datadir/kadm5.acl
    key_stash_file = $kdc_datadir/_k5.$realm
}!);

mkdir $kdc_datadir or die;

# Ensure that we use test's config and cache files, not global ones.
$ENV{'KRB5_CONFIG'}      = $krb5_conf;
$ENV{'KRB5_KDC_PROFILE'} = $kdc_conf;
$ENV{'KRB5CCNAME'}       = $krb5_cache;

my $service_principal = "$ENV{with_krb_srvnam}/$host";

system_or_bail $kdb5_util, 'create', '-s', '-P', 'secret0';

my $test1_password = 'secret1';
system_or_bail $kadmin_local, '-q', "addprinc -pw $test1_password test1";

system_or_bail $kadmin_local, '-q', "addprinc -randkey $service_principal";
system_or_bail $kadmin_local, '-q', "ktadd -k $keytab $service_principal";

system_or_bail $krb5kdc, '-P', $kdc_pidfile;

END
{
	kill 'INT', `cat $kdc_pidfile` if -f $kdc_pidfile;
}

note "setting up PostgreSQL instance";

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
listen_addresses = '$hostaddr'
krb_server_keyfile = '$keytab'
log_connections = on
lc_messages = 'C'
});
$node->start;

$node->safe_psql('postgres', 'CREATE USER test1;');

# Set up a table for SYSTEM_USER parallel worker testing.
$node->safe_psql('postgres',
	"CREATE TABLE ids (id) AS SELECT 'gss:test1\@$realm' FROM generate_series(1, 10);"
);

$node->safe_psql('postgres', 'GRANT SELECT ON ids TO public;');

note "running tests";

# Test connection success or failure, and if success, that query returns true.
sub test_access
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $role, $query, $expected_res, $gssencmode, $test_name,
		@expect_log_msgs)
	  = @_;

	# need to connect over TCP/IP for Kerberos
	my $connstr = $node->connstr('postgres')
	  . " user=$role host=$host hostaddr=$hostaddr $gssencmode";

	my %params = (sql => $query,);

	if (@expect_log_msgs)
	{
		# Match every message literally.
		my @regexes = map { qr/\Q$_\E/ } @expect_log_msgs;

		$params{log_like} = \@regexes;
	}

	if ($expected_res eq 0)
	{
		# The result is assumed to match "true", or "t", here.
		$params{expected_stdout} = qr/^t$/;

		$node->connect_ok($connstr, $test_name, %params);
	}
	else
	{
		$node->connect_fails($connstr, $test_name, %params);
	}
}

# As above, but test for an arbitrary query result.
sub test_query
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $role, $query, $expected, $gssencmode, $test_name) = @_;

	# need to connect over TCP/IP for Kerberos
	my $connstr = $node->connstr('postgres')
	  . " user=$role host=$host hostaddr=$hostaddr $gssencmode";

	$node->connect_ok(
		$connstr, $test_name,
		sql             => $query,
		expected_stdout => $expected);
	return;
}

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{host all all $hostaddr/32 gss map=mymap});
$node->restart;

test_access($node, 'test1', 'SELECT true', 2, '', 'fails without ticket');

run_log [ $kinit, 'test1' ], \$test1_password or BAIL_OUT($?);

test_access(
	$node,
	'test1',
	'SELECT true',
	2,
	'',
	'fails without mapping',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"no match in usermap \"mymap\" for user \"test1\"");

$node->append_conf('pg_ident.conf', qq{mymap  /^(.*)\@$realm\$  \\1});
$node->restart;

test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	'',
	'succeeds with mapping with default gssencmode and host hba',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, principal=test1\@$realm)"
);

test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	'gssencmode=prefer',
	'succeeds with GSS-encrypted access preferred with host hba',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, principal=test1\@$realm)"
);
test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	'gssencmode=require',
	'succeeds with GSS-encrypted access required with host hba',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, principal=test1\@$realm)"
);

# Test that we can transport a reasonable amount of data.
test_query(
	$node,
	'test1',
	'SELECT * FROM generate_series(1, 100000);',
	qr/^1\n.*\n1024\n.*\n9999\n.*\n100000$/s,
	'gssencmode=require',
	'receiving 100K lines works');

test_query(
	$node,
	'test1',
	"CREATE TEMP TABLE mytab (f1 int primary key);\n"
	  . "COPY mytab FROM STDIN;\n"
	  . join("\n", (1 .. 100000))
	  . "\n\\.\n"
	  . "SELECT COUNT(*) FROM mytab;",
	qr/^100000$/s,
	'gssencmode=require',
	'sending 100K lines works');

# Test that SYSTEM_USER works.
test_query($node, 'test1', 'SELECT SYSTEM_USER;',
	qr/^gss:test1\@$realm$/s, 'gssencmode=require', 'testing system_user');

# Test that SYSTEM_USER works with parallel workers.
test_query(
	$node,
	'test1', qq(
	SET min_parallel_table_scan_size TO 0;
	SET parallel_setup_cost TO 0;
	SET parallel_tuple_cost TO 0;
	SET max_parallel_workers_per_gather TO 2;
	SELECT bool_and(SYSTEM_USER = id) FROM ids;),
	qr/^t$/s,
	'gssencmode=require',
	'testing system_user with parallel workers');

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{hostgssenc all all $hostaddr/32 gss map=mymap});
$node->restart;

test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	'gssencmode=prefer',
	'succeeds with GSS-encrypted access preferred and hostgssenc hba',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, principal=test1\@$realm)"
);
test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	'gssencmode=require',
	'succeeds with GSS-encrypted access required and hostgssenc hba',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, principal=test1\@$realm)"
);
test_access($node, 'test1', 'SELECT true', 2, 'gssencmode=disable',
	'fails with GSS encryption disabled and hostgssenc hba');

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{hostnogssenc all all $hostaddr/32 gss map=mymap});
$node->restart;

test_access(
	$node,
	'test1',
	'SELECT gss_authenticated and not encrypted from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	'gssencmode=prefer',
	'succeeds with GSS-encrypted access preferred and hostnogssenc hba, but no encryption',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=no, principal=test1\@$realm)"
);
test_access($node, 'test1', 'SELECT true', 2, 'gssencmode=require',
	'fails with GSS-encrypted access required and hostnogssenc hba');
test_access(
	$node,
	'test1',
	'SELECT gss_authenticated and not encrypted from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	'gssencmode=disable',
	'succeeds with GSS encryption disabled and hostnogssenc hba',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=no, principal=test1\@$realm)"
);

truncate($node->data_dir . '/pg_ident.conf', 0);
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{host all all $hostaddr/32 gss include_realm=0});
$node->restart;

test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	'',
	'succeeds with include_realm=0 and defaults',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, principal=test1\@$realm)"
);

# Reset pg_hba.conf, and cause a usermap failure with an authentication
# that has passed.
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{host all all $hostaddr/32 gss include_realm=0 krb_realm=EXAMPLE.ORG});
$node->restart;

test_access(
	$node,
	'test1',
	'SELECT true',
	2,
	'',
	'fails with wrong krb_realm, but still authenticates',
	"connection authenticated: identity=\"test1\@$realm\" method=gss");

done_testing();
