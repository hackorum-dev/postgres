
# Copyright (c) 2021, PostgreSQL Global Development Group

use strict;
use warnings;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

if ($ENV{with_ldap} eq 'yes')
{
	plan tests => 61;
}
else
{
	plan skip_all => 'LDAP not supported by this build';
}

my ($slapd, $ldap_bin_dir, $ldap_schema_dir);

$ldap_bin_dir = undef;    # usually in PATH

if ($^O eq 'darwin' && -d '/usr/local/opt/openldap')
{
	# typical paths for Homebrew
	$slapd           = '/usr/local/opt/openldap/libexec/slapd';
	$ldap_schema_dir = '/usr/local/etc/openldap/schema';
}
elsif ($^O eq 'darwin' && -d '/opt/local/etc/openldap')
{
	# typical paths for MacPorts
	$slapd           = '/opt/local/libexec/slapd';
	$ldap_schema_dir = '/opt/local/etc/openldap/schema';
}
elsif ($^O eq 'linux')
{
	$slapd           = '/usr/sbin/slapd';
	$ldap_schema_dir = '/etc/ldap/schema' if -d '/etc/ldap/schema';
	$ldap_schema_dir = '/etc/openldap/schema' if -d '/etc/openldap/schema';
}
elsif ($^O eq 'freebsd')
{
	$slapd           = '/usr/local/libexec/slapd';
	$ldap_schema_dir = '/usr/local/etc/openldap/schema';
}

# make your own edits here
#$slapd = '';
#$ldap_bin_dir = '';
#$ldap_schema_dir = '';

$ENV{PATH} = "$ldap_bin_dir:$ENV{PATH}" if $ldap_bin_dir;

my $ldap_datadir  = "${PostgreSQL::Test::Utils::tmp_check}/openldap-data";
my $slapd_certs   = "${PostgreSQL::Test::Utils::tmp_check}/slapd-certs";
my $slapd_conf    = "${PostgreSQL::Test::Utils::tmp_check}/slapd.conf";
my $slapd_pidfile = "${PostgreSQL::Test::Utils::tmp_check}/slapd.pid";
my $slapd_logfile = "${PostgreSQL::Test::Utils::log_path}/slapd.log";
my $ldap_conf     = "${PostgreSQL::Test::Utils::tmp_check}/ldap.conf";
my $ldap_server   = 'localhost';
my $ldap_port     = PostgreSQL::Test::Cluster::get_free_port();
my $ldaps_port    = PostgreSQL::Test::Cluster::get_free_port();
my $ldap_url      = "ldap://$ldap_server:$ldap_port";
my $ldaps_url     = "ldaps://$ldap_server:$ldaps_port";
my $ldap_basedn   = 'dc=example,dc=net';
my $ldap_rootdn   = 'cn=Manager,dc=example,dc=net';
my $ldap_rootpw   = 'secret';
my $ldap_pwfile   = "${PostgreSQL::Test::Utils::tmp_check}/ldappassword";

note "setting up slapd";

append_to_file(
	$slapd_conf,
	qq{include $ldap_schema_dir/core.schema
include $ldap_schema_dir/cosine.schema
include $ldap_schema_dir/nis.schema
include $ldap_schema_dir/inetorgperson.schema
include postgresuser.schema

pidfile $slapd_pidfile
logfile $slapd_logfile
loglevel conns filter stats

access to *
        by * read
        by anonymous auth

database ldif
directory $ldap_datadir

TLSCACertificateFile $slapd_certs/ca.crt
TLSCertificateFile $slapd_certs/server.crt
TLSCertificateKeyFile $slapd_certs/server.key

suffix "dc=example,dc=net"
rootdn "$ldap_rootdn"
rootpw $ldap_rootpw});

# don't bother to check the server's cert (though perhaps we should)
append_to_file(
	$ldap_conf,
	qq{TLS_REQCERT never
});

mkdir $ldap_datadir or die;
mkdir $slapd_certs  or die;

system_or_bail "openssl", "req", "-new", "-nodes", "-keyout",
  "$slapd_certs/ca.key", "-x509", "-out", "$slapd_certs/ca.crt", "-subj",
  "/CN=CA";
system_or_bail "openssl", "req", "-new", "-nodes", "-keyout",
  "$slapd_certs/server.key", "-out", "$slapd_certs/server.csr", "-subj",
  "/CN=server";
system_or_bail "openssl", "x509", "-req", "-in", "$slapd_certs/server.csr",
  "-CA", "$slapd_certs/ca.crt", "-CAkey", "$slapd_certs/ca.key",
  "-CAcreateserial", "-out", "$slapd_certs/server.crt";

sub start_slapd
{
	system_or_bail $slapd, '-f', $slapd_conf, '-h', "$ldap_url $ldaps_url";
}

sub wait_for_slapd
{
	my ($url) = @_;

	# wait until slapd accepts requests
	my $retries = 0;
	while (1)
	{
		last
		  if (
			system_log(
				"ldapsearch", "-sbase",
				"-H",         $url,
				"-b",         $ldap_basedn,
				"-D",         $ldap_rootdn,
				"-y",         $ldap_pwfile,
				"-n",         "'objectclass=*'") == 0);
		die "cannot connect to slapd" if ++$retries >= 300;
		note "waiting for slapd to accept requests...";
		Time::HiRes::usleep(1000000);
	}
}

sub stop_slapd
{
	kill 'INT', `cat $slapd_pidfile` if -f $slapd_pidfile;
}

sub restart_slapd
{
	my ($url) = @_;

	stop_slapd();
	start_slapd();
	wait_for_slapd($url);
}

start_slapd();

END
{
	stop_slapd();
}

append_to_file($ldap_pwfile, $ldap_rootpw);
chmod 0600, $ldap_pwfile or die;

wait_for_slapd($ldap_url);

$ENV{'LDAPURI'}    = $ldap_url;
$ENV{'LDAPBINDDN'} = $ldap_rootdn;
$ENV{'LDAPCONF'}   = $ldap_conf;

note "loading LDAP data";

system_or_bail 'ldapadd',    '-x', '-y', $ldap_pwfile, '-f', 'authdata.ldif';
system_or_bail 'ldappasswd', '-x', '-y', $ldap_pwfile, '-s', 'secret1',
  'uid=test1,dc=example,dc=net';
system_or_bail 'ldappasswd', '-x', '-y', $ldap_pwfile, '-s', 'secret2',
  'uid=test2,dc=example,dc=net';

note "setting up PostgreSQL instance";

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf('postgresql.conf', "log_connections = on\n");
$node->start;

$node->safe_psql('postgres', 'CREATE USER test0;');
$node->safe_psql('postgres', 'CREATE USER test1;');
$node->safe_psql('postgres', 'CREATE USER "test2@example.net";');

my @databases = ( 'anon', 'noattrs', 'badmap', 'starttls', 'bindpw', 'bindscram', 'bindcert' );
foreach my $db (@databases)
{
	$node->safe_psql('postgres', "CREATE DATABASE $db");
}

note "running tests";

sub test_access
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $role, $expected_res, $test_name, %params) = @_;
	my $connstr = "user=$role";

	if ($expected_res eq 0)
	{
		$node->connect_ok($connstr, $test_name, %params);
	}
	else
	{
		# No checks of the error message, only the status code.
		$node->connect_fails($connstr, $test_name, %params);
	}
}

note "simple bind";

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{local all all ldap ldapserver=$ldap_server ldapport=$ldap_port ldapprefix="uid=" ldapsuffix=",dc=example,dc=net"}
);
$node->restart;

$ENV{"PGPASSWORD"} = 'wrong';
test_access(
	$node, 'test0', 2,
	'simple bind authentication fails if user not found in LDAP',
	log_unlike => [qr/connection authenticated:/]);
test_access(
	$node, 'test1', 2,
	'simple bind authentication fails with wrong password',
	log_unlike => [qr/connection authenticated:/]);

$ENV{"PGPASSWORD"} = 'secret1';
test_access(
	$node, 'test1', 0,
	'simple bind authentication succeeds',
	log_like => [
		qr/connection authenticated: identity="uid=test1,dc=example,dc=net" method=ldap/
	],);

note "search+bind";

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{local all all ldap ldapserver=$ldap_server ldapport=$ldap_port ldapbasedn="$ldap_basedn"}
);
$node->restart;

$ENV{"PGPASSWORD"} = 'wrong';
test_access($node, 'test0', 2,
	'search+bind authentication fails if user not found in LDAP');
test_access($node, 'test1', 2,
	'search+bind authentication fails with wrong password');
$ENV{"PGPASSWORD"} = 'secret1';
test_access(
	$node, 'test1', 0,
	'search+bind authentication succeeds',
	log_like => [
		qr/connection authenticated: identity="uid=test1,dc=example,dc=net" method=ldap/
	],);

note "multiple servers";

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{local all all ldap ldapserver="$ldap_server $ldap_server" ldapport=$ldap_port ldapbasedn="$ldap_basedn"}
);
$node->restart;

$ENV{"PGPASSWORD"} = 'wrong';
test_access($node, 'test0', 2,
	'search+bind authentication fails if user not found in LDAP');
test_access($node, 'test1', 2,
	'search+bind authentication fails with wrong password');
$ENV{"PGPASSWORD"} = 'secret1';
test_access($node, 'test1', 0, 'search+bind authentication succeeds');

note "LDAP URLs";

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{local all all ldap ldapurl="$ldap_url/$ldap_basedn?uid?sub"});
$node->restart;

$ENV{"PGPASSWORD"} = 'wrong';
test_access($node, 'test0', 2,
	'search+bind with LDAP URL authentication fails if user not found in LDAP'
);
test_access($node, 'test1', 2,
	'search+bind with LDAP URL authentication fails with wrong password');
$ENV{"PGPASSWORD"} = 'secret1';
test_access($node, 'test1', 0,
	'search+bind with LDAP URL authentication succeeds');

note "search filters";

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{local all all ldap ldapserver=$ldap_server ldapport=$ldap_port ldapbasedn="$ldap_basedn" ldapsearchfilter="(|(uid=\$username)(mail=\$username))"}
);
$node->restart;

$ENV{"PGPASSWORD"} = 'secret1';
test_access(
	$node, 'test1', 0,
	'search filter finds by uid',
	log_like => [
		qr/connection authenticated: identity="uid=test1,dc=example,dc=net" method=ldap/
	],);
$ENV{"PGPASSWORD"} = 'secret2';
test_access(
	$node,
	'test2@example.net',
	0,
	'search filter finds by mail',
	log_like => [
		qr/connection authenticated: identity="uid=test2,dc=example,dc=net" method=ldap/
	],);

note "search filters in LDAP URLs";

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{local all all ldap ldapurl="$ldap_url/$ldap_basedn??sub?(|(uid=\$username)(mail=\$username))"}
);
$node->restart;

$ENV{"PGPASSWORD"} = 'secret1';
test_access($node, 'test1', 0, 'search filter finds by uid');
$ENV{"PGPASSWORD"} = 'secret2';
test_access($node, 'test2@example.net', 0, 'search filter finds by mail');

# This is not documented: You can combine ldapurl and other ldap*
# settings.  ldapurl is always parsed first, then the other settings
# override.  It might be useful in a case like this.
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{local all all ldap ldapurl="$ldap_url/$ldap_basedn??sub" ldapsearchfilter="(|(uid=\$username)(mail=\$username))"}
);
$node->restart;

$ENV{"PGPASSWORD"} = 'secret1';
test_access($node, 'test1', 0, 'combined LDAP URL and search filter');

note "diagnostic message";

# note bad ldapprefix with a question mark that triggers a diagnostic message
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{local all all ldap ldapserver=$ldap_server ldapport=$ldap_port ldapprefix="?uid=" ldapsuffix=""}
);
$node->restart;

$ENV{"PGPASSWORD"} = 'secret1';
test_access($node, 'test1', 2, 'any attempt fails due to bad search pattern');

note "TLS";

# request StartTLS with ldaptls=1
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{local all all ldap ldapserver=$ldap_server ldapport=$ldap_port ldapbasedn="$ldap_basedn" ldapsearchfilter="(uid=\$username)" ldaptls=1}
);
$node->restart;

$ENV{"PGPASSWORD"} = 'secret1';
test_access($node, 'test1', 0, 'StartTLS');

# request LDAPS with ldapscheme=ldaps
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{local all all ldap ldapserver=$ldap_server ldapscheme=ldaps ldapport=$ldaps_port ldapbasedn="$ldap_basedn" ldapsearchfilter="(uid=\$username)"}
);
$node->restart;

$ENV{"PGPASSWORD"} = 'secret1';
test_access($node, 'test1', 0, 'LDAPS');

# request LDAPS with ldapurl=ldaps://...
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{local all all ldap ldapurl="$ldaps_url/$ldap_basedn??sub?(uid=\$username)"}
);
$node->restart;

$ENV{"PGPASSWORD"} = 'secret1';
test_access($node, 'test1', 0, 'LDAPS with URL');

# bad combination of LDAPS and StartTLS
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{local all all ldap ldapurl="$ldaps_url/$ldap_basedn??sub?(uid=\$username)" ldaptls=1}
);
$node->restart;

$ENV{"PGPASSWORD"} = 'secret1';
test_access($node, 'test1', 2, 'bad combination of LDAPS and StartTLS');

note 'LDAP attribute ident mapping';

delete $ENV{"PGPASSWORD"};

# We'll use cert auth for mapping. Reuse the LDAP CA we already have for
# simplicity (this is a nonsensical setup in practice).
system_or_bail "openssl", "req", "-new", "-nodes",
  "-keyout", "$slapd_certs/test1-client.key",
  "-out", "$slapd_certs/test1-client.csr",
  "-subj", "/DC=net/DC=example/CN=test1";
system_or_bail "openssl", "x509", "-req",
  "-in", "$slapd_certs/test1-client.csr",
  "-CA", "$slapd_certs/ca.crt", "-CAkey", "$slapd_certs/ca.key",
  "-CAcreateserial", "-out", "$slapd_certs/test1-client.crt";
system_or_bail "openssl", "req", "-new", "-nodes",
  "-keyout", "$slapd_certs/test2-client.key",
  "-out", "$slapd_certs/test2-client.csr",
  "-subj", "/DC=net/DC=example/CN=test2";
system_or_bail "openssl", "x509", "-req",
  "-in", "$slapd_certs/test2-client.csr",
  "-CA", "$slapd_certs/ca.crt", "-CAkey", "$slapd_certs/ca.key",
  "-CAcreateserial", "-out", "$slapd_certs/test2-client.crt";

my $SERVERHOSTADDR = '127.0.0.1';

$node->append_conf('postgresql.conf', qq{
listen_addresses = '$SERVERHOSTADDR'
ssl = on
ssl_ca_file = '$slapd_certs/ca.crt'
ssl_cert_file = '$slapd_certs/server.crt'
ssl_key_file = '$slapd_certs/server.key'
});

# XXX check the other SSL tests' security mitigations for hostssl
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{
# TYPE   DATABASE  USER  ADDRESS  METHOD  OPTIONS
hostssl  anon      all   all      cert    ldapmap=ldap
hostssl  noattrs   all   all      cert    ldapmap=noattrs
hostssl  badmap    all   all      cert    ldapmap=badmap
hostssl  starttls  all   all      cert    ldapmap=ldap ldaptls=1
hostssl  bindpw    all   all      cert    ldapmap=ldap ldaptls=1 ldapbinddn="$ldap_rootdn" ldapbindpasswd="$ldap_rootpw"
hostssl  bindscram all   all      cert    ldapmap=ldap ldaptls=1 ldapsaslmechs=scram-sha-1 ldapbinddn=Manager ldapbindpasswd="$ldap_rootpw"
hostssl  bindcert  all   all      cert    ldapmap=ldap ldaptls=1 ldapsaslmechs=external
});

unlink($node->data_dir . '/pg_ident.conf');
$node->append_conf('pg_ident.conf',
	qq{
# This query matches only postgresUser entries, and returns their postgresRole
# attributes.
ldap    /^(.*)\$ "$ldap_url/$ldap_basedn?postgresRole?sub?(&(objectClass=postgresUser)(uid=\\1))"

# This query matches any object with the given uid, so it can return entries
# with no attribute values.
noattrs /^(.*)\$ "$ldap_url/$ldap_basedn?postgresRole?sub?(&(objectClass=*)(uid=\\1))"

# This query matches multiple DNs and should fail.
badmap  /^       "$ldap_url/$ldap_basedn?postgresRole?sub?(objectClass=inetOrgPerson)"
});

$node->restart;

my $common_connstr =
	"host=server hostaddr=$SERVERHOSTADDR sslmode=verify-full " .
	"sslrootcert='$slapd_certs/ca.crt' " .
	"sslcert='$slapd_certs/test2-client.crt' " .
	"sslkey='$slapd_certs/test2-client.key'";

$node->connect_ok(
	"$common_connstr dbname=anon user=test0",
    "ldapmap succeeds with role attribute");

$node->connect_fails(
	"$common_connstr dbname=anon user=test1",
	"ldapmap fails without matching role attribute",
	log_like => [
		qr/no match in ldapmap "ldap" for user "test1" authenticated as ".*"/,
	]);

$node->connect_ok(
	"$common_connstr dbname=anon user='test2\@example.net'",
	"ldapmap succeeds with another role attribute");

$node->connect_fails(
	"$common_connstr dbname=badmap user=test0",
	"ldapmap fails if query matches multiple DNs",
	log_like => [
		qr/query matched multiple DNs/,
		qr/no match in ldapmap "badmap" for user "test0" authenticated as ".*"/,
	]);

# Switch to the test1 client cert, which does not have a corresponding
# postgresUser in the LDAP tree.
$common_connstr =
	"host=server hostaddr=$SERVERHOSTADDR sslmode=verify-full " .
	"sslrootcert='$slapd_certs/ca.crt' " .
	"sslcert='$slapd_certs/test1-client.crt' " .
	"sslkey='$slapd_certs/test1-client.key'";

$node->connect_fails(
	"$common_connstr dbname=anon user=test1",
	"ldapmap fails if query matches no DNs",
	log_like => [
		qr/query returned no entries/,
		qr/no match in ldapmap "ldap" for user "test1" authenticated as ".*"/,
	]);

$node->connect_fails(
	"$common_connstr dbname=noattrs user=test1",
	"ldapmap fails if entry has no attributes",
	log_like => [
		qr/entry had no role attributes/,
		qr/no match in ldapmap "noattrs" for user "test1" authenticated as ".*"/,
	]);

note 'LDAP ident mapping with StartTLS';

# Force the use of TLS for connections from this point onward.
append_to_file(
	$slapd_conf,
	qq{
security tls=128
});

restart_slapd($ldaps_url);

$common_connstr =
	"host=server hostaddr=$SERVERHOSTADDR sslmode=verify-full " .
	"sslrootcert='$slapd_certs/ca.crt' " .
	"sslcert='$slapd_certs/test2-client.crt' " .
	"sslkey='$slapd_certs/test2-client.key'";

$node->connect_fails(
	"$common_connstr dbname=anon user=test0",
	"anonymous ldapmap binding fails with StartTLS enforcement",
	log_like => [
		qr/connection authenticated:/,
		qr/LDAP role search failed on server .*: Confidentiality required/,
		qr/no match in ldapmap "ldap" for user "test0" authenticated as ".*"/,
	]);

$node->connect_ok(
	"$common_connstr dbname=starttls user=test0",
	"ldapmap works with StartTLS");

note 'LDAP ident mapping with bind password';

# Force the use of authenticated connections from this point onward.
append_to_file(
	$slapd_conf,
	qq{require authc
});

restart_slapd($ldaps_url);

$node->connect_fails(
	"$common_connstr dbname=starttls user=test0",
	"anonymous ldapmap binding fails",
	log_like => [
		qr/connection authenticated:/,
		qr/LDAP diagnostics: authentication required/,
		qr/no match in ldapmap "ldap" for user "test0" authenticated as ".*"/,
	]);

$node->connect_ok(
	"$common_connstr dbname=bindpw user=test0",
	"ldapmap works with bind password");

note 'LDAP ident mapping with SCRAM binding';

$node->connect_fails(
	"$common_connstr dbname=bindscram user=test0",
	"ldapmap can't perform SCRAM authentication without server setup",
	log_like => [
		qr/could not perform SASL bind on server .*: Invalid credentials/,
		qr/user not found: no secret in database/,
	]);

# Map the SCRAM-specific authentication DN to our root user.
append_to_file(
	$slapd_conf,
	qq{
authz-regexp
	uid=Manager,cn=SCRAM-SHA-1,cn=auth
	cn=Manager,dc=example,dc=net
});

restart_slapd($ldaps_url);

$node->connect_ok(
	"$common_connstr dbname=bindscram user=test0",
	"ldapmap works with SCRAM authentication to LDAP server");

note 'LDAP ident mapping with client certificate';

# Set up a certificate for the root user.
system_or_bail "openssl", "req", "-new", "-nodes",
  "-keyout", "$slapd_certs/root-client.key",
  "-out", "$slapd_certs/root-client.csr",
  "-subj", "/DC=net/DC=example/CN=Manager";
system_or_bail "openssl", "x509", "-req", "-in", "$slapd_certs/root-client.csr",
  "-CA", "$slapd_certs/ca.crt", "-CAkey", "$slapd_certs/ca.key",
  "-CAcreateserial", "-out", "$slapd_certs/root-client.crt";

$ENV{'LDAPTLS_CERT'} = "$slapd_certs/root-client.crt";
$ENV{'LDAPTLS_KEY'}  = "$slapd_certs/root-client.key";

# Force the use of client certificates from this point onward.
append_to_file(
	$slapd_conf,
	qq{TLSVerifyClient demand
});

restart_slapd($ldaps_url);

$node->connect_fails(
	"$common_connstr dbname=bindpw user=test0",
	"ldapmap with bind password fails without client certificate",
	log_like => [
		qr/connection authenticated:/,
		qr/could not perform initial LDAP bind for ldapbinddn "cn=Manager,dc=example,dc=net" on server ".*": Can't contact LDAP server/,
		qr/no match in ldapmap "ldap" for user "test0" authenticated as ".*"/,
	]);

# The server needs to be restarted to pick up all the above LDAPTLS_* settings
# from the environment.
$node->restart;

$node->connect_ok(
	"$common_connstr dbname=bindpw user=test0",
	"ldapmap works with bind certificate");

$node->connect_ok(
	"$common_connstr dbname=bindcert user=test0",
	"ldapmap works with client certificate authentication");

note 'LDAP group ident mapping';
# TODO
