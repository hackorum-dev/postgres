# Copyright (c) 2026, PostgreSQL Global Development Group

# Test multi-certificate support via ssl_cert_files/ssl_key_files

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use SSL::Server;

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
my $SERVERHOSTADDR = '127.0.0.1';
my $SERVERHOSTCIDR = '127.0.0.1/32';

#### Set up the server.

note "setting up data directory";
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;

$ENV{PGHOST} = $node->host;
$ENV{PGPORT} = $node->port;
$node->start;

$ssl_server->configure_test_server_for_ssl($node, $SERVERHOSTADDR,
	$SERVERHOSTCIDR, 'trust', );

my $pgdata = $node->data_dir;

#### Generate ECDSA cert signed by the test server CA.

my $ssl_dir = "$FindBin::RealBin/../ssl";
my $ecdsa_key = "$pgdata/server-ecdsa.key";
my $ecdsa_csr = "$pgdata/server-ecdsa.csr";
my $ecdsa_crt = "$pgdata/server-ecdsa.crt";

note "generating ECDSA server certificate";

system("openssl ecparam -genkey -name prime256v1 -out $ecdsa_key 2>/dev/null") == 0
	or die "failed to generate ECDSA key";
system("openssl req -new -key $ecdsa_key -out $ecdsa_csr -subj '/CN=localhost' -batch 2>/dev/null") == 0
	or die "failed to generate ECDSA CSR";
system("openssl x509 -req -in $ecdsa_csr -CA $ssl_dir/server_ca.crt -CAkey $ssl_dir/server_ca.key "
	. "-CAserial $pgdata/ca.srl -CAcreateserial -out $ecdsa_crt -days 3650 2>/dev/null") == 0
	or die "failed to sign ECDSA cert";
chmod 0600, $ecdsa_key;
unlink $ecdsa_csr;

# Helper to rewrite sslconfig.conf from scratch
sub write_sslconfig
{
	my ($node, %opts) = @_;
	my $conf = $node->data_dir . '/sslconfig.conf';
	unlink($conf);
	$node->append_conf('sslconfig.conf', "ssl=on");
	$node->append_conf('sslconfig.conf',
		"ssl_ca_file='root+client_ca.crt'");
	# Use singular ssl_cert_file/ssl_key_file as primary unless overridden
	if (!exists $opts{ssl_cert_file})
	{
		$node->append_conf('sslconfig.conf',
			"ssl_cert_file='server-cn-only.crt'");
	}
	if (!exists $opts{ssl_key_file})
	{
		$node->append_conf('sslconfig.conf',
			"ssl_key_file='server-cn-only.key'");
	}
	foreach my $key (sort keys %opts)
	{
		$node->append_conf('sslconfig.conf', "$key=$opts{$key}");
	}
}

#### Configure server with multi-cert via ssl_cert_files.

note "configuring server with ssl_cert_files (RSA + ECDSA)";

$ssl_server->switch_server_cert($node,
	certfile => 'server-cn-only',
	cafile => 'root+client_ca',
	restart => 'no');

$node->append_conf('sslconfig.conf',
	"ssl_cert_files='$pgdata/server-cn-only.crt, $ecdsa_crt'");
$node->append_conf('sslconfig.conf',
	"ssl_key_files='$pgdata/server-cn-only.key, $ecdsa_key'");

$node->restart;

#### Tests.

my $common_connstr = "sslrootcert=invalid hostaddr=$SERVERHOSTADDR host=localhost "
	. "user=ssltestuser dbname=trustdb sslmode=require";

# Test 1: Basic connectivity with multi-cert
note "testing basic connectivity with multi-cert";
$node->connect_ok(
	"$common_connstr sslcert=invalid",
	"connect with multi-cert via default negotiation",
	sql => "SELECT 1");

# Test 2: Verify the GUC parameters are set
my $result = $node->safe_psql('trustdb',
	"SHOW ssl_cert_files",
	connstr => "$common_connstr sslcert=invalid");
like($result, qr/server-cn-only\.crt/, 'ssl_cert_files includes RSA cert');
like($result, qr/server-ecdsa\.crt/, 'ssl_cert_files includes ECDSA cert');

# Test 3: Verify both cipher types work via openssl s_client (TLS 1.2)
note "testing RSA cipher via openssl s_client";
my $openssl_rsa = `echo "QUIT" | openssl s_client -connect $SERVERHOSTADDR:${\$node->port} -starttls postgres -tls1_2 -cipher ECDHE-RSA-AES256-GCM-SHA384 2>&1`;
like($openssl_rsa, qr/ECDHE-RSA-AES256-GCM-SHA384/, 'RSA cipher negotiates successfully');

note "testing ECDSA cipher via openssl s_client";
my $openssl_ecdsa = `echo "QUIT" | openssl s_client -connect $SERVERHOSTADDR:${\$node->port} -starttls postgres -tls1_2 -cipher ECDHE-ECDSA-AES256-GCM-SHA384 2>&1`;
like($openssl_ecdsa, qr/ECDHE-ECDSA-AES256-GCM-SHA384/, 'ECDSA cipher negotiates successfully');

# Test 4: Verify correct cert type is served for each cipher
note "verifying RSA cert served for RSA cipher";
my $rsa_cert = `echo "QUIT" | openssl s_client -connect $SERVERHOSTADDR:${\$node->port} -starttls postgres -tls1_2 -cipher ECDHE-RSA-AES256-GCM-SHA384 2>&1 | openssl x509 -noout -text 2>/dev/null`;
like($rsa_cert, qr/rsaEncryption/, 'RSA cert served for RSA cipher');

note "verifying ECDSA cert served for ECDSA cipher";
my $ecdsa_cert = `echo "QUIT" | openssl s_client -connect $SERVERHOSTADDR:${\$node->port} -starttls postgres -tls1_2 -cipher ECDHE-ECDSA-AES256-GCM-SHA384 2>&1 | openssl x509 -noout -text 2>/dev/null`;
like($ecdsa_cert, qr/id-ecPublicKey/, 'ECDSA cert served for ECDSA cipher');

# Test 5: TLS 1.3 connectivity with multi-cert
note "testing TLS 1.3 connection with multi-cert";
$node->connect_ok(
	"$common_connstr sslcert=invalid",
	"connect via TLS 1.3 with multi-cert (default negotiation)",
	sql => "SELECT 1");

# Test 6: TLS 1.3 HelloRetryRequest with multi-cert
# Force HRR by configuring the server to accept only secp384r1 while
# the client offers X25519 first.  The server sends HelloRetryRequest
# asking for secp384r1, and the client retries.  This exercises the
# ssl_update_ssl() code path being called twice in one handshake.
note "testing TLS 1.3 HelloRetryRequest with multi-cert";

$node->append_conf('sslconfig.conf', "ssl_groups='secp384r1'");
$node->reload;
sleep(1);

my $openssl_hrr = `echo "QUIT" | openssl s_client -connect $SERVERHOSTADDR:${\$node->port} -starttls postgres -tls1_3 -groups X25519:secp384r1 2>&1`;
like($openssl_hrr, qr/TLSv1\.3/,
	'TLS 1.3 connection succeeds after HelloRetryRequest with multi-cert');

# Restore default groups
$node->append_conf('sslconfig.conf', "ssl_groups='X25519:prime256v1:secp384r1:secp521r1:ffdhe2048'");
$node->reload;
sleep(1);

# Test 7: Mismatched list lengths
note "testing mismatched ssl_cert_files/ssl_key_files lengths";

write_sslconfig($node,
	ssl_cert_files => "'$pgdata/server-cn-only.crt, $ecdsa_crt'",
	ssl_key_files => "'$pgdata/server-cn-only.key'");

$result = $node->restart(fail_ok => 1);
is($result, 0, 'restart fails with mismatched list lengths');

my $log = slurp_file($node->logfile);
like($log, qr/ssl_cert_files has \d+ entries but ssl_key_files has \d+ entries/,
	'log contains expected error for mismatched list lengths');

# Test 8: ssl_cert_files without ssl_key_files
note "testing ssl_cert_files without ssl_key_files";

write_sslconfig($node,
	ssl_cert_files => "'$pgdata/server-cn-only.crt'");

$result = $node->restart(fail_ok => 1);
is($result, 0, 'restart fails with ssl_cert_files set but ssl_key_files empty');

$log = slurp_file($node->logfile);
like($log, qr/ssl_cert_files is set but ssl_key_files is not/,
	'log contains expected error for missing ssl_key_files');

# Test 9: ssl_key_files without ssl_cert_files
note "testing ssl_key_files without ssl_cert_files";

write_sslconfig($node,
	ssl_key_files => "'$ecdsa_key'");

$result = $node->restart(fail_ok => 1);
is($result, 0, 'restart fails with ssl_key_files set but ssl_cert_files empty');

$log = slurp_file($node->logfile);
like($log, qr/ssl_key_files is set but ssl_cert_files is not/,
	'log contains expected error for missing ssl_cert_files');

# Test 10: Bad certificate file path
note "testing bad certificate file path in ssl_cert_files";

write_sslconfig($node,
	ssl_cert_files => "'/nonexistent/cert.crt'",
	ssl_key_files => "'$ecdsa_key'");

$result = $node->restart(fail_ok => 1);
is($result, 0, 'restart fails with bad certificate file path');

$log = slurp_file($node->logfile);
like($log, qr/could not load server certificate file.*nonexistent/,
	'log contains expected error for bad certificate path');

# Test 11: Certificate/key type mismatch
note "testing certificate/key type mismatch in ssl_cert_files";

write_sslconfig($node,
	ssl_cert_files => "'$pgdata/server-cn-only.crt'",
	ssl_key_files => "'$ecdsa_key'");

$result = $node->restart(fail_ok => 1);
is($result, 0, 'restart fails with cert/key type mismatch');

$log = slurp_file($node->logfile);
like($log, qr/check of private key failed/,
	'log contains expected error for cert/key mismatch');

# Test 12: Single cert mode (no ssl_cert_files) still works
note "testing single cert mode (no ssl_cert_files)";

write_sslconfig($node);
$node->start;

$node->connect_ok(
	"$common_connstr sslcert=invalid",
	"connect with single RSA cert (no ssl_cert_files)",
	sql => "SELECT 1");

# Test 13: ssl_cert_files takes precedence over ssl_cert_file
note "testing ssl_cert_files takes precedence over ssl_cert_file";

# ssl_cert_file points to RSA cert (server-cn-only), but ssl_cert_files
# includes ECDSA.  Verify ECDSA is available, proving ssl_cert_files won.
$node->append_conf('sslconfig.conf',
	"ssl_cert_files='$pgdata/server-cn-only.crt, $ecdsa_crt'");
$node->append_conf('sslconfig.conf',
	"ssl_key_files='$pgdata/server-cn-only.key, $ecdsa_key'");
$node->reload;
sleep(1);

my $openssl_precedence = `echo "QUIT" | openssl s_client -connect $SERVERHOSTADDR:${\$node->port} -starttls postgres -tls1_2 -cipher ECDHE-ECDSA-AES256-GCM-SHA384 2>&1`;
like($openssl_precedence, qr/ECDHE-ECDSA-AES256-GCM-SHA384/,
	'ssl_cert_files takes precedence: ECDSA available despite ssl_cert_file being RSA only');

$log = slurp_file($node->logfile);
like($log, qr/ssl_cert_file and ssl_key_file are overridden by ssl_cert_files and ssl_key_files/,
	'log contains warning that ssl_cert_file is overridden');

# Restore single cert for subsequent tests
write_sslconfig($node);
$node->reload;
sleep(1);

# Test 14: SIGHUP reload adds multi-cert
note "testing SIGHUP reload adds multi-cert";

$node->append_conf('sslconfig.conf',
	"ssl_cert_files='$pgdata/server-cn-only.crt, $ecdsa_crt'");
$node->append_conf('sslconfig.conf',
	"ssl_key_files='$pgdata/server-cn-only.key, $ecdsa_key'");
$node->reload;
sleep(1);

my $openssl_after_reload = `echo "QUIT" | openssl s_client -connect $SERVERHOSTADDR:${\$node->port} -starttls postgres -tls1_2 -cipher ECDHE-ECDSA-AES256-GCM-SHA384 2>&1`;
like($openssl_after_reload, qr/ECDHE-ECDSA-AES256-GCM-SHA384/,
	'ECDSA cipher works after SIGHUP reload');

# Test 15: SIGHUP reload removes multi-cert
note "testing SIGHUP reload removes multi-cert";

write_sslconfig($node);
$node->reload;
sleep(1);

my $openssl_after_remove = `echo "QUIT" | openssl s_client -connect $SERVERHOSTADDR:${\$node->port} -starttls postgres -tls1_2 -cipher ECDHE-ECDSA-AES256-GCM-SHA384 2>&1`;
unlike($openssl_after_remove, qr/ECDHE-ECDSA-AES256-GCM-SHA384/,
	'ECDSA cipher fails after multi-cert removed via SIGHUP');

$node->connect_ok(
	"$common_connstr sslcert=invalid",
	"RSA connection works after multi-cert removed via SIGHUP",
	sql => "SELECT 1");

done_testing();
