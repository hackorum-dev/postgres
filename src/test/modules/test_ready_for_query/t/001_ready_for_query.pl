# Copyright (c) 2026, PostgreSQL Global Development Group

# Test ReadyForQuery wire protocol extension and hook

use strict;
use warnings;

# Set a hard test-level timeout of 60 seconds to detect and handle any hanging condition
$SIG{ALRM} = sub { die "Test timed out after 60 seconds\n"; };
alarm(60);

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'test_ready_for_query'\nmax_prepared_transactions = 10\n"
);
$node->start;

# 1. Test SQL-level GUC settings and extension management
my $guc_val = $node->safe_psql('postgres', 'SHOW ready_for_query_message;');
is($guc_val, 'plain', 'default GUC value is plain');

$guc_val = $node->safe_psql('postgres', "SET ready_for_query_message = 'rich'; SHOW ready_for_query_message;");
is($guc_val, 'rich', 'GUC can be set to rich');

$guc_val = $node->safe_psql('postgres', "SET ready_for_query_message = 'plain'; SHOW ready_for_query_message;");
is($guc_val, 'plain', 'GUC can be reset to plain');

my ($ret, $stdout, $stderr) = $node->psql('postgres', "SET ready_for_query_message = 'invalid';");
isnt($ret, 0, 'invalid GUC value rejected');
like($stderr, qr/invalid value for parameter "ready_for_query_message"/, 'error message on invalid GUC value');

$node->safe_psql('postgres', 'CREATE EXTENSION test_ready_for_query;');
$node->safe_psql('postgres', 'DROP EXTENSION test_ready_for_query;');

# Helper function to read exactly $n bytes from a socket
sub read_exact
{
	my ($sock, $len) = @_;
	my $buf = '';
	while (length($buf) < $len)
	{
		my $chunk;
		my $n = $sock->sysread($chunk, $len - length($buf));
		die "unexpected EOF on socket" if !defined($n) || $n == 0;
		$buf .= $chunk;
	}
	return $buf;
}

# Helper function to send simple query 'Q' message
sub send_query
{
	my ($sock, $sql) = @_;
	my $payload = $sql . "\0";
	my $len = 4 + length($payload);
	my $msg = 'Q' . pack('N', $len) . $payload;
	$sock->syswrite($msg);
}

# Helper function to read messages until ReadyForQuery ('Z')
sub read_until_ready_for_query
{
	my ($sock) = @_;
	while (1)
	{
		my $mtype = read_exact($sock, 1);
		my $raw_len = read_exact($sock, 4);
		my $mlen = unpack('N', $raw_len);
		my $body_len = $mlen - 4;
		my $body = $body_len > 0 ? read_exact($sock, $body_len) : '';

		if ($mtype eq 'Z')
		{
			my $tx_status = substr($body, 0, 1);
			my %kv = ();
			my $offset = 1;
			while ($offset < length($body))
			{
				my $klen = ord(substr($body, $offset++, 1));
				my $key = substr($body, $offset, $klen);
				$offset += $klen;
				my $vlen = ord(substr($body, $offset++, 1));
				my $val = $vlen > 0 ? substr($body, $offset, $vlen) : '';
				$offset += $vlen;
				$kv{$key} = $val;
			}
			return ($mlen, $tx_status, \%kv);
		}
	}
}

if (!$node->raw_connect_works())
{
	plan skip_all => "this test requires working raw_connect()";
}

# 2. Connect via raw socket and perform StartupPacket
my $dbuser = $node->safe_psql('postgres', 'SELECT current_user;');
my $sock = $node->raw_connect();

my $startup_body = pack('N', 196608) . "user\0$dbuser\0database\0postgres\0\0";
my $startup_msg = pack('N', 4 + length($startup_body)) . $startup_body;
$sock->syswrite($startup_msg);

# Read initial ReadyForQuery (default 'plain' mode)
my ($len1, $tx1, $kv1) = read_until_ready_for_query($sock);
is($len1, 5, "default ReadyForQuery message length is 5 (plain mode)");
is($tx1, 'I', "initial transaction status is Idle ('I')");
is(scalar(keys %$kv1), 0, "no extra key-value pairs in plain mode");

# 3. Switch to 'rich' mode
send_query($sock, "SET ready_for_query_message = 'rich';");
my ($len2, $tx2, $kv2) = read_until_ready_for_query($sock);
cmp_ok($len2, '>', 5, "rich mode ReadyForQuery message length > 5");
is($tx2, 'I', "transaction status is Idle ('I')");
is($kv2->{'T'}, '0', "T=0 when no temp tables exist");
is($kv2->{'H'}, '0', "H=0 when no with-hold cursors exist");
is($kv2->{'P'}, '0', "P=0 when no prepared transactions exist");
ok(defined $kv2->{'L'}, "L (LSN) key is present in rich mode");

# 4. Create temporary table and verify 'T' and hook-injected 'temp_tables_info'
send_query($sock, "CREATE TEMP TABLE t_temp (id int);");
my ($len3, $tx3, $kv3) = read_until_ready_for_query($sock);
is($kv3->{'T'}, '1', "T=1 after temporary table creation");
is($kv3->{'temp_tables_info'}, 'has_temp_namespace', "hook injected temp_tables_info key");

# 5. Create WITH HOLD cursor and verify 'H' and commit LSN 'L'
send_query($sock, "BEGIN; DECLARE cur CURSOR WITH HOLD FOR SELECT 1; COMMIT;");
my ($len4, $tx4, $kv4) = read_until_ready_for_query($sock);
is($kv4->{'H'}, '1', "H=1 after creating cursor WITH HOLD");
isnt($kv4->{'L'}, '0/0', "commit LSN is non-zero after COMMIT");

# 6. Two-phase prepared transaction 'P'
send_query($sock, "BEGIN; CREATE TABLE t_prep(x int); PREPARE TRANSACTION 't1';");
my ($len_prep, $tx_prep, $kv_prep) = read_until_ready_for_query($sock);
is($kv_prep->{'P'}, '1', "P=1 after PREPARE TRANSACTION");

send_query($sock, "COMMIT PREPARED 't1';");
my ($len_comm, $tx_comm, $kv_comm) = read_until_ready_for_query($sock);
is($kv_comm->{'P'}, '0', "P=0 after COMMIT PREPARED");

# 7. Switch back to 'plain' mode
send_query($sock, "SET ready_for_query_message = 'plain';");
my ($len5, $tx5, $kv5) = read_until_ready_for_query($sock);
is($len5, 5, "reverting to plain mode restores ReadyForQuery length to 5");
is(scalar(keys %$kv5), 0, "no extra key-value pairs in plain mode");

# 8. Test with StartupPacket containing options="-c ready_for_query_message=rich"
my $sock2 = $node->raw_connect();
my $startup_body2 = pack('N', 196608) . "user\0$dbuser\0database\0postgres\0options\0-c ready_for_query_message=rich\0\0";
my $startup_msg2 = pack('N', 4 + length($startup_body2)) . $startup_body2;
$sock2->syswrite($startup_msg2);

my ($len_opt, $tx_opt, $kv_opt) = read_until_ready_for_query($sock2);
cmp_ok($len_opt, '>', 5, "startup option enables rich mode on initial ReadyForQuery");
is($kv_opt->{'T'}, '0', "T=0 on fresh session");
is($kv_opt->{'H'}, '0', "H=0 on fresh session");

close($sock);
close($sock2);
$node->stop;

alarm(0);
done_testing();
