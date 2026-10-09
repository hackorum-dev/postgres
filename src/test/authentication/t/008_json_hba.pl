# Copyright (c) 2026, PostgreSQL Global Development Group

# Test the JSON format of the client authentication rules and user name
# maps, in the files named by hba_file and ident_file.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (!$use_unix_sockets)
{
	plan skip_all => "authentication tests cannot run without Unix-domain sockets";
}

my $node = PostgreSQL::Test::Cluster->new('json_hba');
$node->init;
my $datadir = $node->data_dir;
my $hba_json = "$datadir/pg_hba.json";
my $ident_json = "$datadir/pg_ident.json";

sub write_file
{
	my ($path, $content) = @_;
	open my $fh, '>', $path or die "cannot write $path: $!";
	print $fh $content;
	close $fh;
}

# $hba is the JSON text of the "hba" value.
sub set_hba
{
	my ($hba) = @_;
	write_file($hba_json, qq({"hba": $hba}\n));
}

# $ident is the JSON text of the "ident" value.
sub set_ident
{
	my ($ident) = @_;
	write_file($ident_json, qq({"ident": $ident}\n));
}

$node->append_conf('postgresql.conf', <<"EOF");
listen_addresses = '127.0.0.1'
hba_file = '$hba_json'
ident_file = '$ident_json'
EOF
set_hba(
	'{"children": [{"transport": "local", "auth": {"method": "trust"}}]}');
set_ident('{}');
$node->start;

# With EXEC_BACKEND, each new backend loads the rules from the files
# itself, rather than inheriting what the postmaster loaded.
my $exec_backend = $node->safe_psql('postgres', 'SHOW debug_exec_backend');

$node->safe_psql(
	'postgres', q{
	SET password_encryption = 'scram-sha-256';
	CREATE ROLE alice LOGIN PASSWORD 'pass';
	CREATE ROLE bob LOGIN PASSWORD 'pass';
	CREATE ROLE jacob LOGIN PASSWORD 'pass';
	CREATE ROLE admins NOLOGIN;
	GRANT admins TO bob;
	CREATE ROLE "all" LOGIN PASSWORD 'pass';
});

# The three-level example from the proposal, with scram standing in for
# LDAP so it can be tested without an LDAP server: everyone scram,
# admins additionally get a per-rule setting, jacob is rejected.
set_hba(<<'EOF');
{"children": [
  {"transport": "local", "auth": {"method": "trust"}},
  {"address": "127.0.0.1/32",
   "auth": {"method": "scram-sha-256"},
   "settings": {"work_mem": "11MB"},
   "children": [
     {"users": "+admins",
      "settings": {"work_mem": "12MB", "log_connections": "all"}},
     {"users": "jacob", "auth": {"method": "reject"}}
   ]}
]}
EOF
$node->reload;

my $res = $node->safe_psql(
	'postgres', q{
	SELECT rule_number, type, user_name, auth_method, node_path,
	       settings::text, error IS NULL
	FROM pg_hba_file_rules ORDER BY rule_number});
is( $res,
	join(
		"\n",
		'1|local|{all}|trust|$.hba.children[0]||t',
		'2|host|{+admins}|scram-sha-256|$.hba.children[1].children[0]|'
		  . '{"work_mem": "12MB", "log_connections": "all"}|t',
		'3|host|{jacob}|reject|$.hba.children[1].children[1]|'
		  . '{"work_mem": "11MB"}|t',
		'4|host|{all}|scram-sha-256|$.hba.children[1]|'
		  . '{"work_mem": "11MB"}|t'),
	'tree compiled depth-first, children before own rule, settings merged');

$ENV{PGPASSWORD} = 'pass';
$node->connect_ok(
	'host=127.0.0.1 dbname=postgres user=alice',
	'everyone else authenticates with scram',
	sql => 'SHOW work_mem',
	expected_stdout => qr/^11MB$/m);
$node->connect_ok(
	'host=127.0.0.1 dbname=postgres user=bob',
	'admin inherits method, overrides setting',
	sql => 'SHOW work_mem',
	expected_stdout => qr/^12MB$/m);
$node->connect_fails(
	'host=127.0.0.1 dbname=postgres user=jacob',
	'jacob is rejected by the more specific child',
	expected_stderr => qr/rejects connection/);
$node->connect_ok(
	'host=127.0.0.1 dbname=postgres user=alice',
	'hba settings show source hba',
	sql => q{SELECT source FROM pg_settings WHERE name = 'work_mem'},
	expected_stdout => qr/^hba$/m);

# per-role settings rank above hba settings
$node->safe_psql('postgres', "ALTER ROLE alice SET work_mem = '21MB'");
$node->connect_ok(
	'host=127.0.0.1 dbname=postgres user=alice',
	'role setting overrides hba setting',
	sql => 'SHOW work_mem',
	expected_stdout => qr/^21MB$/m);
$node->safe_psql('postgres', 'ALTER ROLE alice RESET work_mem');

# setting names match regardless of case when merged, as parameters do
set_hba(<<'EOF');
{"children": [
  {"transport": "local", "auth": {"method": "trust"}},
  {"address": "127.0.0.1/32", "auth": {"method": "scram-sha-256"},
   "settings": {"work_mem": "1MB"},
   "children": [
     {"settings": {"Work_Mem": "2MB"}, "children": [
        {"users": "alice", "settings": {"work_mem": "3MB"}}
     ]}
   ]}
]}
EOF
$node->reload;
$res = $node->safe_psql(
	'postgres', q{
	SELECT settings::text FROM pg_hba_file_rules WHERE rule_number = 2});
is($res, '{"work_mem": "3MB"}', 'setting overridden regardless of case');
$node->connect_ok(
	'host=127.0.0.1 dbname=postgres user=alice',
	'deepest override of a setting wins',
	sql => 'SHOW work_mem',
	expected_stdout => qr/^3MB$/m);

# a value naming a database object that does not exist is accepted, as it
# is for ALTER ROLE SET; the view, loading the file in a connected backend,
# agrees with the postmaster, which cannot look the name up
set_hba(<<'EOF');
{"children": [
  {"transport": "local", "auth": {"method": "trust"}},
  {"address": "127.0.0.1/32", "auth": {"method": "scram-sha-256"},
   "settings": {"default_tablespace": "no_such_space"}}
]}
EOF
$node->reload;
$res = $node->safe_psql(
	'postgres', q{
	SET client_min_messages = warning;
	SELECT error IS NULL FROM pg_hba_file_rules WHERE rule_number = 2});
is($res, 't', 'missing tablespace accepted by the view as by the load');
$node->connect_ok(
	'host=127.0.0.1 dbname=postgres user=alice',
	'missing tablespace applied on faith',
	sql => 'SHOW default_tablespace',
	expected_stdout => qr/^no_such_space$/m);

# The example of the documentation, with scram for LDAP: everyone over
# SSL, admins additionally need a client certificate, jacob is rejected.
# The ssl constraint is inherited, so every rule below it is hostssl,
# which clientcert needs.  This cluster runs without SSL, so the rules
# load with the usual note that they cannot match.
set_hba(<<'EOF');
{"children": [
  {"transport": "local", "auth": {"method": "trust"}},
  {"ssl": "require", "auth": {"method": "scram-sha-256"},
   "children": [
     {"users": "+admins", "auth": {"clientcert": "verify-full"}},
     {"users": "jacob", "auth": {"method": "reject"}}
   ]}
]}
EOF
$res = $node->safe_psql(
	'postgres', q{
	SELECT line_number, type, user_name, auth_method, options,
	       error ~ '^hostssl record cannot match'
	FROM pg_hba_file_rules WHERE type <> 'local' ORDER BY line_number});
is( $res,
	join(
		"\n",
		'3|hostssl|{all}|scram-sha-256||t',
		'5|hostssl|{+admins}|scram-sha-256|{clientcert=verify-full}|t',
		'6|hostssl|{jacob}|reject||t'),
	'documented example loads as hostssl rules');

# auth merge: child without method adds an option, child with method
# replaces (ident, as one of the methods that take a map)
set_hba(<<'EOF');
{"children": [
  {"transport": "local", "auth": {"method": "trust"}},
  {"address": "127.0.0.1/32", "auth": {"method": "ident", "map": "m1"},
   "children": [
     {"users": "alice", "auth": {"method": "trust"}},
     {"users": "bob", "auth": {"map": "m2"}}
   ]}
]}
EOF
$res = $node->safe_psql(
	'postgres', q{
	SELECT rule_number, auth_method, options FROM pg_hba_file_rules
	WHERE rule_number > 1 ORDER BY rule_number});
is($res, qq{2|trust|\n3|ident|{map=m2}\n4|ident|{map=m1}},
	'auth replaced when child sets method, merged otherwise');

# address arrays: one flat rule per element, same node path
set_hba(<<'EOF');
{"children": [
  {"transport": "local", "auth": {"method": "trust"}},
  {"address": ["127.0.0.1/32", "::1/128"], "auth": {"method": "trust"}}
]}
EOF
$res = $node->safe_psql(
	'postgres', q{
	SELECT rule_number, address, node_path FROM pg_hba_file_rules
	WHERE rule_number > 1 ORDER BY rule_number});
is($res, qq{2|127.0.0.1|\$.hba.children[1]\n3|::1|\$.hba.children[1]},
	'address array expands to one rule per element');

# positional import of a legacy file, inheriting settings only; a missing
# optional import is skipped
write_file("$datadir/legacy_hba.conf",
	"host all alice 127.0.0.1/32 trust\n");
set_hba(<<'EOF');
{"children": [
  {"transport": "local", "auth": {"method": "trust"}},
  {"settings": {"work_mem": "13MB"}, "children": [
     {"import": "missing.conf", "optional": true},
     {"import": "legacy_hba.conf"},
     {"address": "127.0.0.1/32", "auth": {"method": "reject"}}
  ]}
]}
EOF
$node->reload;
$res = $node->safe_psql(
	'postgres', q{
	SELECT rule_number, regexp_replace(file_name, '.*/', ''), line_number,
	       user_name, auth_method, node_path IS NULL, settings::text
	FROM pg_hba_file_rules WHERE rule_number > 1 ORDER BY rule_number});
is( $res,
	join(
		"\n",
		'2|legacy_hba.conf|1|{alice}|trust|t|{"work_mem": "13MB"}',
		'3|pg_hba.json|6|{all}|reject|f|{"work_mem": "13MB"}'),
	'import inserts legacy rules at its position with inherited settings');
$node->connect_ok(
	'host=127.0.0.1 dbname=postgres user=alice',
	'legacy rule matched before the sibling after it',
	sql => 'SHOW work_mem',
	expected_stdout => qr/^13MB$/m);
$node->connect_ok(
	'host=127.0.0.1 dbname=postgres user=alice',
	'setting inherited by a legacy rule shows source hba',
	sql => q{SELECT source FROM pg_settings WHERE name = 'work_mem'},
	expected_stdout => qr/^hba$/m);

# an included document adds its rules after the including one; the
# include is relative to the document
mkdir "$datadir/hba.d";
write_file("$datadir/hba.d/10-extra.json", <<'EOF');
{"hba": {"children": [
  {"address": "127.0.0.1/32", "users": "bob", "auth": {"method": "trust"}}
]}}
EOF
write_file($hba_json, <<'EOF');
{"include": ["hba.d/*.json", {"file": "missing.json", "optional": true}],
 "hba": {"children": [
  {"transport": "local", "auth": {"method": "trust"}},
  {"address": "127.0.0.1/32", "users": "alice", "auth": {"method": "trust"}}
]}}
EOF
$node->reload;
$res = $node->safe_psql(
	'postgres', q{
	SELECT rule_number, regexp_replace(file_name, '.*/', ''), line_number,
	       user_name, node_path
	FROM pg_hba_file_rules WHERE rule_number > 1 ORDER BY rule_number});
is( $res,
	join("\n",
		'2|pg_hba.json|4|{alice}|$.hba.children[1]',
		'3|10-extra.json|2|{bob}|$.hba.children[0]'),
	'included document listed after the including one');
$node->connect_ok('host=127.0.0.1 dbname=postgres user=bob',
	'rule from an included document matched');

# a pattern that matches nothing is an error unless optional: losing the
# last file of a directory of rejects must not let the rules after it
# through
mkdir "$datadir/deny.d";
write_file("$datadir/deny.d/10-bob.json", <<'EOF');
{"hba": {"children": [
  {"address": "127.0.0.1/32", "users": "bob", "auth": {"method": "reject"}}
]}}
EOF
write_file($hba_json, <<'EOF');
{"include": ["deny.d/*.json", "hba.d/*.json"],
 "hba": {"children": [
  {"transport": "local", "auth": {"method": "trust"}}
]}}
EOF
my $log_offset = -s $node->logfile;
$node->reload;
$node->wait_for_log(qr/reloading configuration files/, $log_offset);
$node->connect_fails('host=127.0.0.1 dbname=postgres user=bob',
	'rejected by the rule from the included directory');

$log_offset = -s $node->logfile;
unlink "$datadir/deny.d/10-bob.json";
$node->reload;
$node->wait_for_log(
	qr/include pattern "deny\.d\/\*\.json" matches no files/, $log_offset);
$node->wait_for_log(qr/pg_hba\.json was not reloaded/, $log_offset);
$node->connect_fails('host=127.0.0.1 dbname=postgres user=bob',
	'unmatched pattern fails the load, keeping the old rules');

write_file($hba_json, <<'EOF');
{"include": [{"file": "deny.d/*.json", "optional": true}, "hba.d/*.json"],
 "hba": {"children": [
  {"transport": "local", "auth": {"method": "trust"}}
]}}
EOF
$log_offset = -s $node->logfile;
$node->reload;
$node->wait_for_log(qr/reloading configuration files/, $log_offset);
$node->connect_ok('host=127.0.0.1 dbname=postgres user=bob',
	'unmatched optional pattern reads nothing');
rmdir "$datadir/deny.d";

# an optional include may be missing, but not unreadable
SKIP:
{
	skip 'unix-style permissions not supported on Windows', 1 if $windows_os;

	my $log_offset = -s $node->logfile;
	write_file($hba_json, <<'EOF');
{"include": [{"file": "hba.d/10-extra.json", "optional": true}],
 "hba": {"children": [
  {"transport": "local", "auth": {"method": "trust"}}
]}}
EOF
	chmod 0000, "$datadir/hba.d/10-extra.json" or die $!;
	$node->reload;
	$node->wait_for_log(
		qr/could not open configuration file ".*10-extra\.json": Permission denied/,
		$log_offset);
	$node->wait_for_log(qr/pg_hba\.json was not reloaded/, $log_offset);
	chmod 0644, "$datadir/hba.d/10-extra.json" or die $!;
	ok(1, 'unreadable optional include fails the load');
}

# @file in a core field is read relative to the JSON file
write_file("$datadir/roles.txt", "alice\nbob\n");
set_hba(<<'EOF');
{"children": [
  {"transport": "local", "auth": {"method": "trust"}},
  {"address": "127.0.0.1/32", "users": "@roles.txt",
   "auth": {"method": "trust"}},
  {"address": "127.0.0.1/32", "auth": {"method": "reject"}}
]}
EOF
$node->reload;
$res = $node->safe_psql('postgres',
	q{SELECT user_name FROM pg_hba_file_rules WHERE rule_number = 2});
is($res, '{alice,bob}', '@file expanded into the user list');
$node->connect_ok('host=127.0.0.1 dbname=postgres user=bob',
	'user listed in the @file is matched');
$node->connect_fails(
	'host=127.0.0.1 dbname=postgres user=jacob',
	'user not listed in the @file is rejected',
	expected_stderr => qr/rejects connection/);

# quoted literal keeps the role named "all" apart from the keyword
set_hba(<<'EOF');
{"children": [
  {"transport": "local", "auth": {"method": "trust"}},
  {"address": "127.0.0.1/32", "users": "\"all\"",
   "auth": {"method": "trust"}},
  {"address": "127.0.0.1/32", "auth": {"method": "reject"}}
]}
EOF
$node->reload;
$node->connect_ok('host=127.0.0.1 dbname=postgres user=all',
	'quoted user token matches role named all');
$node->connect_fails(
	'host=127.0.0.1 dbname=postgres user=alice',
	'quoted user token is not the keyword',
	expected_stderr => qr/rejects connection/);

# per-rule authentication_timeout re-arms the timer
set_hba(<<'EOF');
{"children": [
  {"transport": "local", "auth": {"method": "trust"}},
  {"address": "127.0.0.1/32", "auth": {"method": "scram-sha-256"},
   "settings": {"authentication_timeout": "7"}}
]}
EOF
$node->reload;
$node->connect_ok(
	'host=127.0.0.1 dbname=postgres user=alice',
	'per-rule authentication_timeout applied',
	sql => 'SHOW authentication_timeout',
	expected_stdout => qr/^7s$/m);

# a list setting as an array, quoted as the parameter needs
set_hba(<<'EOF');
{"children": [
  {"transport": "local", "auth": {"method": "trust"}},
  {"address": "127.0.0.1/32", "auth": {"method": "trust"},
   "settings": {"search_path": ["My Schema", "public"]}}
]}
EOF
$node->reload;
$node->connect_ok(
	'host=127.0.0.1 dbname=postgres user=alice',
	'array setting joined into a list',
	sql => 'SHOW search_path',
	expected_stdout => qr/^"My Schema", public$/m);

# rejected shapes, one per rule, visible in the view; queried from a
# session opened beforehand, since a backend that loads the rules itself
# cannot start on them
write_file("$datadir/empty.txt", "");
my $session = $node->background_psql('postgres', on_error_stop => 0);
set_hba(<<'EOF');
{"children": [
  {"transport": "local", "auth": {"method": "trust"}},
  {"transport": "local", "address": "127.0.0.1/32",
   "auth": {"method": "trust"}},
  {"ssl": "require", "gss": "require", "auth": {"method": "trust"}},
  {"bogus": 1, "auth": {"method": "trust"}},
  {"users": 42, "auth": {"method": "trust"}},
  {"import": "legacy_hba.conf", "children": []},
  {"users": "alice"},
  {"settings": {"work_mem": "1MB"}, "children": []},
  {"auth": {"method": "trust"}, "settings": {"no_such_guc": "1"}},
  {"auth": {"method": "trust"}, "settings": {"shared_buffers": "1GB"}},
  {"auth": {"method": "trust"}, "settings": {"work_mem": "lots"}},
  {"auth": {"method": "scrim"}},
  {"users": "@missing.txt", "auth": {"method": "trust"}},
  {"users": "@empty.txt", "auth": {"method": "trust"}},
  {"transport": "serial", "children": [
     {"transport": "tcp", "auth": {"method": "trust"}}
  ]},
  {"ssl": "forbid", "auth": {"method": "trust"}, "children": [
     {"transport": "local"}
  ]},
  {"address": ["127.0.0.1/32", "10.0.0.1"], "auth": {"method": "trust"}}
]}
EOF
$res = $session->query_safe(
	q{SELECT line_number, error FROM pg_hba_file_rules
	  WHERE error IS NOT NULL ORDER BY line_number});
like(
	$res,
	qr/"address", "ssl" and "gss" cannot be used with transport "local"/,
	'local with address rejected');
like($res, qr/"ssl" and "gss" cannot both be constrained/,
	'ssl+gss rejected');
like($res, qr/unrecognized key "bogus"/, 'unrecognized key rejected');
like(
	$res,
	qr/"users" must be a string or a non-empty array of strings/,
	'wrong type rejected');
like($res, qr/import node cannot have "children"/,
	'children on import rejected');
is(scalar(() = $res =~ /leaf node has no authentication method/g),
	2, 'leaf without method rejected, empty children array is a leaf');
like($res, qr/unrecognized configuration parameter "no_such_guc"/,
	'unknown GUC rejected');
like($res, qr/"shared_buffers" cannot be changed without restarting/,
	'postmaster GUC rejected');
like($res, qr/invalid value for parameter "work_mem"/,
	'bad GUC value rejected');
like($res, qr/invalid authentication method "scrim"/,
	'parse_hba_line errors pass through');
like($res, qr/could not open file ".*missing\.txt"/,
	'missing @file rejected');
like($res, qr/end-of-line before authentication method/,
	'empty @file drops the field, as in pg_hba.conf');
like($res, qr/invalid value for "transport": "serial"/,
	'bad transport on grouping node rejected');
like($res, qr/inherited "ssl" cannot be used with transport "local"/,
	'inherited constraint on local child rejected');
like($res, qr/address "10\.0\.0\.1" must be in CIDR notation/,
	'address without prefix length rejected');
is(scalar(() = $res =~ /\n/g) + 1, 16, 'one error per bad rule');

# a rejected reload keeps the rules loaded before
$node->reload;
$node->wait_for_log(qr/pg_hba\.json was not reloaded/);
SKIP:
{
	skip 'new backends load the rules themselves with EXEC_BACKEND', 3
	  if $exec_backend eq 'on';

	$node->connect_ok(
		'host=127.0.0.1 dbname=postgres user=alice',
		'old rules still in effect after a rejected reload',
		sql => 'SHOW search_path',
		expected_stdout => qr/^"My Schema", public$/m);
}

# problems with the document itself fail the load as a whole, and the
# view reports them instead of returning rows
foreach my $case (
	[ '{"hba": {"children": []}, "ident": {"maps": {}}}',
		qr/unrecognized key "ident"/ ],
	[ '{"hba": {"children": []}, "work_mem": "1MB"}',
		qr/unrecognized key "work_mem"/ ],
	[ '{"hba": {"children": []}, "include": ["legacy_hba.conf"]}',
		qr/included file ".*legacy_hba\.conf" is not a JSON file/ ],
	[ '{"hba": {"children": []}, "include": ["missing.json"]}',
		qr/could not open file ".*missing\.json"/ ],
	[ '{"hba": []}', qr/"hba" must be an object with a "children" array/ ],
	[ '{"hba": {"children": [}}', qr/Expected JSON value/ ])
{
	my ($doc, $re) = @$case;

	write_file($hba_json, "$doc\n");
	$session->{stderr} = '';
	$session->query('SELECT * FROM pg_hba_file_rules');
	my $err = $session->{stderr};
	like($err, qr/could not load configuration file ".*pg_hba\.json"/,
		"hba document $doc: view fails");
	like($err, $re, "hba document $doc: problem reported");
}
$session->quit;

# ident maps, tested with the peer method over the Unix socket; only
# template1 goes through the map, so the test's own connections to
# postgres keep working whatever the maps say
my $sysuser = (getpwuid($<))[0];
write_file("$datadir/mapped.txt", "carol\n");
set_hba(<<'EOF');
{"children": [
   {"transport": "local", "databases": "template1",
    "auth": {"method": "peer", "map": "m"}},
   {"transport": "local", "auth": {"method": "trust"}}
]}
EOF
set_ident(<<"EOF");
{
   "import": [{"file": "missing_ident.conf", "optional": true},
              "legacy_ident.conf"],
   "maps": {"m": {"$sysuser": ["alice", "\@mapped.txt"],
                  "/^(.*)\$": "\\\\1"}}
}
EOF
write_file("$datadir/legacy_ident.conf", "m $sysuser bob\n");
$node->reload;
$node->safe_psql('postgres', 'CREATE ROLE carol LOGIN');
$res = $node->safe_psql(
	'postgres', q{
	SELECT map_number, regexp_replace(file_name, '.*/', ''), line_number,
	       map_name, sys_name, pg_username, error IS NULL
	FROM pg_ident_file_mappings ORDER BY map_number});
is( $res,
	join(
		"\n",
		"1|legacy_ident.conf|1|m|$sysuser|bob|t",
		"2|pg_ident.json|4|m|$sysuser|alice|t",
		"3|pg_ident.json|4|m|$sysuser|carol|t",
		"4|pg_ident.json|5|m|/^(.*)\$|\\1|t"),
	'imported mappings first, then one mapping per value element');

$log_offset = -s $node->logfile;
$node->psql('template1', '', connstr => 'dbname=template1 user=alice');
my $peer_supported = !$node->log_contains(
	qr/peer authentication is not supported on this platform/,
	$log_offset);

SKIP:
{
	skip 'peer authentication is not supported on this platform', 5
	  unless $peer_supported;

	$node->connect_ok('dbname=template1 user=alice',
		'peer through a JSON map');
	$node->connect_ok('dbname=template1 user=bob',
		'peer through an imported legacy map');
	$node->connect_ok('dbname=template1 user=carol',
		'peer through a pg user name read from an @file');
	$node->connect_ok("dbname=template1 user=$sysuser",
		'peer through a regex key with \\1 substitution');
	$node->connect_fails(
		'dbname=template1 user=jacob',
		'system user not mapped to the requested role',
		expected_stderr => qr/Peer authentication failed/);
}

# the same map in an included document adds to it
write_file("$datadir/ident_extra.json",
	qq({"ident": {"maps": {"m": {"$sysuser": "jacob"}}}}\n));
write_file($ident_json, <<"EOF");
{"include": ["ident_extra.json"],
 "ident": {"maps": {"m": {"$sysuser": "alice"}}}}
EOF
$node->reload;
SKIP:
{
	skip 'peer authentication is not supported on this platform', 1
	  unless $peer_supported;
	$node->connect_ok('dbname=template1 user=jacob',
		'mapping from an included document concatenated into the map');
}

$res = $node->safe_psql(
	'postgres', q{
	SELECT map_number, regexp_replace(file_name, '.*/', ''), line_number,
	       pg_username
	FROM pg_ident_file_mappings ORDER BY map_number});
is( $res,
	join("\n", "1|pg_ident.json|2|alice", "2|ident_extra.json|1|jacob"),
	'included document listed after the including one');

# per-mapping errors are rows in the view; the import of a missing file
# too
set_ident(<<"EOF");
{
   "import": "missing_ident.conf",
   "maps": {"m": {"a": 1, "b": [1], "c": [], "d": "/([",
                  "e": "\@empty.txt"}}
}
EOF
$res = $node->safe_psql(
	'postgres', q{
	SELECT map_number IS NULL, line_number, error FROM pg_ident_file_mappings
	ORDER BY line_number, error});
like($res, qr/^t\|2\|could not open file ".*missing_ident\.conf"/m,
	'missing import file reported');
is( scalar(
		() = $res =~
		  /^t\|3\|"[abc]" must be a string or a non-empty array of strings/mg),
	3, 'non-string, non-string element and empty array rejected');
like($res, qr/^t\|3\|invalid regular expression/m,
	'parse_ident_line errors pass through');
like($res, qr/^t\|4\|missing entry at end of line/m,
	'empty @file drops the field, as in pg_ident.conf');
is(scalar(() = $res =~ /\n/g) + 1, 6, 'one row per bad mapping or import');
$log_offset = -s $node->logfile;
$node->reload;
$node->wait_for_log(qr/pg_ident\.json was not reloaded/, $log_offset);

# wrong section shapes fail the whole load
foreach my $case (
	[ '[]', qr/"ident" must be an object/ ],
	[ '{"maps": {}, "bogus": 1}', qr/unrecognized key "bogus"/ ],
	[ '{"maps": 1}', qr/"maps" must be an object/ ],
	[ '{"maps": {"m": "x"}}', qr/map "m" must be an object/ ],
	[
		'{"import": [1]}',
		qr/"import" must be a string, an object, or an array of these/
	],
	[ '{"import": {"file": "x", "bogus": 1}}', qr/unrecognized key "bogus"/ ],
	[ '{"import": {"file": "x", "optional": 1}}',
		qr/"optional" must be a boolean/ ])
{
	my ($ident, $re) = @$case;

	set_ident($ident);
	my ($ret, $out, $err) =
	  $node->psql('postgres', 'SELECT * FROM pg_ident_file_mappings');
	like($err, qr/could not load configuration file ".*pg_ident\.json"/,
		"ident $ident: view fails");
	like($err, $re, "ident $ident: shape reported");
}

# a document without rules: startup fails
$node->stop;
write_file($hba_json, "{}\n");
my $ret = $node->start(fail_ok => 1);
is($ret, 0, 'hba document without rules fails to start');
ok($node->log_contains(qr/contains no entries/), 'no entries reported');

done_testing();
