# Copyright (c) 2026, PostgreSQL Global Development Group

# Test the JSON format of the SNI hosts configuration in the file named
# by hosts_file, and the pg_hosts_file_entries view for both formats.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use SSL::Server;

# This is the hostaddr used to connect to the server. This cannot be a
# hostname, because the server certificate is always for the domain
# postgresql-ssl-regression.test.
my $SERVERHOSTADDR = '127.0.0.1';
# This is the pattern to use in pg_hba.conf to match incoming connections.
my $SERVERHOSTCIDR = '127.0.0.1/32';

if ($ENV{with_ssl} ne 'openssl')
{
	plan skip_all => 'OpenSSL not supported by this build';
}

if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bssl\b/)
{
	plan skip_all =>
	  'Potentially unsafe test SSL not enabled in PG_TEST_EXTRA';
}

my $ssl_server = SSL::Server->new();

if ($ssl_server->is_libressl)
{
	plan skip_all => 'SNI not supported when building with LibreSSL';
}

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;

# PGHOST is enforced here to set up the node, subsequent connections
# will use a dedicated connection string.
$ENV{PGHOST} = $node->host;
$ENV{PGPORT} = $node->port;
$node->start;

$ssl_server->configure_test_server_for_ssl($node, $SERVERHOSTADDR,
	$SERVERHOSTCIDR, 'trust');

$ssl_server->switch_server_cert($node, certfile => 'server-cn-only');

my $connstr =
  "user=ssltestuser dbname=trustdb hostaddr=$SERVERHOSTADDR sslsni=1";

# pg_hba.conf only admits SSL connections, so the view is queried over one,
# as the (superuser) OS user.  The default entry serves the same certificate
# throughout, so no SNI name is needed.
my $query_connstr = "dbname=trustdb hostaddr=$SERVERHOSTADDR sslsni=0 "
  . "sslmode=require sslrootcert=ssl/root+server_ca.crt";

sub view_query
{
	my ($sql) = @_;
	return $node->safe_psql('trustdb', $sql, connstr => $query_connstr);
}

# With EXEC_BACKEND, each new backend loads the SSL configuration from the
# files itself, and goes without SSL while they are invalid.  Checks made
# while a file is broken use a session opened beforehand.
my $exec_backend = view_query('SHOW debug_exec_backend');
my $session;

sub session_query
{
	my ($sql) = @_;
	$session->{stderr} = '';
	my $res = $session->query($sql);
	return ($res, $session->{stderr});
}

my $datadir = $node->data_dir;
my $hosts_json = "$datadir/pg_hosts.json";
my $hba_json = "$datadir/pg_hba.json";

sub write_file
{
	my ($path, $content) = @_;
	open my $fh, '>', $path or die "cannot write $path: $!";
	print $fh $content;
	close $fh;
}

# $hosts is the JSON text of the "hosts" value; @extra are further
# include entries (already JSON-quoted).
sub set_hosts
{
	my ($hosts, @extra) = @_;
	my $includes = join(', ', @extra);
	write_file($hosts_json,
		qq({"include": [$includes],\n "hosts": $hosts}\n));
}

##############################################################################
# Legacy pg_hosts.conf in the view
##############################################################################

# The view reads the file itself, so no reload is needed.
$node->append_conf('pg_hosts.conf',
	"* server-cn-only.crt server-cn-only.key");
$node->append_conf('pg_hosts.conf',
	"example.org,example.net server-cn-only.crt server-cn-only.key");
$node->append_conf('pg_hosts.conf', "bad.example.org server-cn-only.crt");

my $res = view_query(
	q{
	SELECT entry_number, hostname, ssl_key_file, error
	FROM pg_hosts_file_entries ORDER BY line_number, hostname});
is( $res,
	qq{1|*|server-cn-only.key|\n}
	  . qq{2|example.net|server-cn-only.key|\n}
	  . qq{2|example.org|server-cn-only.key|\n}
	  . qq{|bad.example.org||missing entry at end of line},
	'pg_hosts_file_entries works for the legacy format');
ok(unlink("$datadir/pg_hosts.conf"), 'legacy hosts file removed');

##############################################################################
# JSON hosts file
##############################################################################

# The hba rules move to a JSON file too, with a rule carrying a setting
# in front of the imported pg_hba.conf, to check that an hba setting
# overrides a hosts setting for the same parameter.
write_file($hba_json, <<"EOF");
{"hba": {"children": [
   {"address": "$SERVERHOSTCIDR", "ssl": "require",
    "databases": "trustdb", "users": "anotheruser",
    "auth": {"method": "trust"},
    "settings": {"work_mem": "12MB"}},
   {"import": "pg_hba.conf"}]}}
EOF
$node->append_conf('postgresql.conf', <<"EOF");
ssl_sni = on
hosts_file = '$hosts_json'
hba_file = '$hba_json'
EOF

# example.org gets its CA from the defaults and carries a setting, the
# default entry overrides the certificate and sets a list parameter from
# an array, the no-SNI entry takes everything from the defaults.
# example.net and example.com come from an imported legacy file that reads
# its host names from an @file.
write_file("$datadir/hosts_extra.conf",
	"\@hostnames.txt server-cn-only+server_ca.crt server-cn-only.key\n");
write_file("$datadir/hostnames.txt", "example.net\nexample.com\n");

my $hosts = <<'EOF';
{"import": ["hosts_extra.conf"],
 "defaults": {"ssl_ca_file": "root_ca.crt",
              "ssl_cert_file": "server-cn-only.crt",
              "ssl_key_file": "server-cn-only.key"},
 "entries": {
   "example.org": {"ssl_cert_file": "server-cn-only+server_ca.crt",
                   "settings": {"work_mem": "11MB"}},
   "*": {"ssl_cert_file": "server-cn-only.crt",
         "settings": {"search_path": ["My Schema", "public"]}},
   "/no_sni/": {}
 }}
EOF
set_hosts($hosts);
$node->restart;

$node->connect_ok(
	"$connstr host=example.org sslrootcert=ssl/root_ca.crt sslmode=verify-ca",
	'per-host certificate selected from JSON entries',
	sql => q{SELECT setting, source FROM pg_settings WHERE name = 'work_mem'},
	expected_stdout => qr/^11264\|hosts$/m);
$node->connect_ok(
	"$connstr host=example.net sslrootcert=ssl/root_ca.crt sslmode=verify-ca",
	'certificate of an imported legacy entry selected',
	sql => q{SELECT setting, source FROM pg_settings WHERE name = 'work_mem'},
	expected_stdout => qr/^4096\|default$/m);
$node->connect_ok(
	"$connstr host=example.com sslrootcert=ssl/root_ca.crt sslmode=verify-ca",
	'certificate of an imported @file host name selected');
$node->connect_ok(
	"$connstr sslsni=0 sslrootcert=ssl/root+server_ca.crt sslmode=require",
	'no-SNI entry built entirely from defaults selected');
$node->connect_ok(
	"$connstr host=other.example.org sslrootcert=ssl/root+server_ca.crt "
	  . "sslmode=require",
	'default entry used without matching name',
	sql => q{SELECT setting, source FROM pg_settings WHERE name = 'work_mem'},
	expected_stdout => qr/^4096\|default$/m);
$node->connect_fails(
	"$connstr host=other.example.org sslrootcert=ssl/root_ca.crt "
	  . "sslmode=verify-ca",
	'default entry has no intermediate CA',
	expected_stderr => qr/certificate verify failed/);

# The hba rule for anotheruser sets the same parameter, and wins.
$node->connect_ok(
	"$connstr user=anotheruser host=example.org "
	  . "sslrootcert=ssl/root_ca.crt sslmode=verify-ca",
	'hba setting overrides the hosts setting',
	sql => q{SELECT setting, source FROM pg_settings WHERE name = 'work_mem'},
	expected_stdout => qr/^12288\|hba$/m);

$res = view_query(
	q{
	SELECT entry_number, regexp_replace(file_name, '.*/', ''), hostname,
	       regexp_replace(ssl_cert_file, '.*/', ''), ssl_ca_file,
	       ssl_passphrase_command_supports_reload, settings::text, error
	FROM pg_hosts_file_entries ORDER BY entry_number, hostname});
is( $res,
	qq{1|hosts_extra.conf|example.com|server-cn-only+server_ca.crt||f||\n}
	  . qq{1|hosts_extra.conf|example.net|server-cn-only+server_ca.crt||f||\n}
	  . qq{2|pg_hosts.json|example.org|server-cn-only+server_ca.crt|}
	  . qq{root_ca.crt|f|{"work_mem": "11MB"}|\n}
	  . qq{3|pg_hosts.json|*|server-cn-only.crt|root_ca.crt|f|}
	  . qq{{"search_path": "\\"My Schema\\", public"}|\n}
	  . qq{4|pg_hosts.json|/no_sni/|server-cn-only.crt|root_ca.crt|f||},
	'pg_hosts_file_entries lists JSON entries with defaults applied');

##############################################################################
# Duplicates and other errors
##############################################################################

# A duplicate hostname across an include is an error, the old
# configuration stays active.  Matching is case-insensitive.
mkdir "$datadir/conf.d";
$session = $node->background_psql(
	'trustdb',
	connstr => $query_connstr,
	on_error_stop => 0);
write_file(
	"$datadir/conf.d/dup.json",
	qq({"hosts": {"entries": {"EXAMPLE.org": )
	  . qq({"ssl_cert_file": "server-cn-only.crt", )
	  . qq("ssl_key_file": "server-cn-only.key"}}}}\n));
set_hosts($hosts, '"conf.d/dup.json"');
my $log_offset = -s $node->logfile;
$node->reload;
$node->wait_for_log(qr/duplicate hosts entry "EXAMPLE.org"/, $log_offset);
SKIP:
{
	skip 'new backends load the SSL configuration with EXEC_BACKEND', 3
	  if $exec_backend eq 'on';

	$node->connect_ok(
		"$connstr host=example.org sslrootcert=ssl/root_ca.crt "
		  . "sslmode=verify-ca",
		'old hosts configuration kept after failed reload',
		sql =>
		  q{SELECT setting, source FROM pg_settings WHERE name = 'work_mem'},
		expected_stdout => qr/^11264\|hosts$/m);
}
($res) = session_query(
	q{SELECT hostname, error FROM pg_hosts_file_entries
	  WHERE error IS NOT NULL});
is($res, 'EXAMPLE.org|duplicate hosts entry "EXAMPLE.org"',
	'duplicate shown in view');

# A duplicate of an imported legacy hostname is caught too.
write_file(
	"$datadir/conf.d/dup.json",
	qq({"hosts": {"entries": {"example.net": )
	  . qq({"ssl_cert_file": "server-cn-only.crt", )
	  . qq("ssl_key_file": "server-cn-only.key"}}}}\n));
($res) = session_query(
	q{SELECT hostname, error FROM pg_hosts_file_entries
	  WHERE error IS NOT NULL});
is($res, 'example.net|duplicate hosts entry "example.net"',
	'duplicate of an imported hostname shown in view');

# Per-entry problems: a non-object entry, an unrecognized key, a wrong
# type, and a setting that does not validate.
write_file(
	"$datadir/conf.d/dup.json",
	qq({"hosts": {"entries": {\n)
	  . qq( "a.example.org": "server-cn-only.crt",\n)
	  . qq( "b.example.org": {"ssl_cert_file": "server-cn-only.crt", )
	  . qq("ssl_key_file": "server-cn-only.key", "ssl_crl_file": "x"},\n)
	  . qq( "c.example.org": {"ssl_cert_file": "server-cn-only.crt", )
	  . qq("ssl_key_file": 1},\n)
	  . qq( "d.example.org": {"ssl_cert_file": "server-cn-only.crt", )
	  . qq("ssl_key_file": "server-cn-only.key", )
	  . qq("settings": {"work_mem": "lots"}}}}}\n));
($res) = session_query(
	q{SELECT hostname, error FROM pg_hosts_file_entries
	  WHERE error IS NOT NULL ORDER BY hostname});
is( $res,
	qq{a.example.org|hosts entry must be an object, not string\n}
	  . qq{b.example.org|unrecognized key "ssl_crl_file"\n}
	  . qq{c.example.org|"ssl_key_file" must be a string\n}
	  . qq{d.example.org|invalid value for parameter "work_mem": "lots"},
	'per-entry errors shown in view');

# A section of the wrong shape is a file-level error, as is a key the
# file cannot hold.
write_file("$datadir/conf.d/dup.json",
	qq({"hosts": {"entries": {}, "rules": []}}\n));
my (undef, $stderr) =
  session_query('SELECT * FROM pg_hosts_file_entries');
like(
	$stderr,
	qr/unrecognized key "rules" \(node \$\.hosts\.rules,/,
	'section shape error reported by the view');
write_file("$datadir/conf.d/dup.json",
	qq({"hosts": {"entries": {}}, "hba": {"children": []}}\n));
(undef, $stderr) = session_query('SELECT * FROM pg_hosts_file_entries');
like(
	$stderr,
	qr/unrecognized key "hba" \(node \$\.hba,/,
	'other section in a hosts file reported by the view');
$session->quit;

# A malformed section, with no other hosts section anywhere, must not
# pass for a missing one: the reload keeps the per-host certificate
# rather than falling back to the ssl_* parameters, and the server does
# not start on it.
unlink "$datadir/conf.d/dup.json";
write_file($hosts_json, qq({"hosts": []}\n));
$log_offset = -s $node->logfile;
$node->reload;
$node->wait_for_log(qr/could not load file .*"hosts" must be an object/,
	$log_offset);
SKIP:
{
	skip 'new backends load the SSL configuration with EXEC_BACKEND', 2
	  if $exec_backend eq 'on';

	$node->connect_ok(
		"$connstr host=example.org sslrootcert=ssl/root_ca.crt "
		  . "sslmode=verify-ca",
		'per-host certificate kept after a reload with a malformed section');
}

$node->stop;
$log_offset = -s $node->logfile;
my $ret = $node->start(fail_ok => 1);
is($ret, 0, 'start fails with a malformed hosts section');
my $log = PostgreSQL::Test::Utils::slurp_file($node->logfile, $log_offset);
like(
	$log,
	qr/"hosts" must be an object/,
	'startup failure names the problem');

# Without a hosts section anywhere, the ssl_* parameters are used.
write_file($hosts_json, "{}\n");
$node->start;
$session = $node->background_psql(
	'trustdb',
	connstr => $query_connstr,
	on_error_stop => 0);

# Without defaults, an entry must name its key itself.  The view reads
# the file as it is, so the running configuration stays as it was.
write_file(
	$hosts_json,
	qq({"hosts": {"entries": {"a.example.org": )
	  . qq({"ssl_cert_file": "server-cn-only.crt"}}}}\n));
($res) = session_query(
	q{SELECT hostname, error FROM pg_hosts_file_entries});
is($res, 'a.example.org|missing required key "ssl_key_file"',
	'missing required key shown in view');
$session->quit;
write_file($hosts_json, "{}\n");
$node->connect_ok(
	"$connstr host=example.org sslrootcert=ssl/root+server_ca.crt "
	  . "sslmode=require",
	'no hosts section falls back to the ssl_* parameters');
$res = view_query('SELECT count(*) FROM pg_hosts_file_entries');
is($res, '0', 'view is empty without a hosts section');

done_testing();
