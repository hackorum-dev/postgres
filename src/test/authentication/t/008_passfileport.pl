
# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Tests for the passfileport connection parameter, which sets the port used as
# the lookup key in the password file independently of the connection port.
# This is what lets a .pgpass entry written for the real server port keep
# matching when the connection is made through an SSH tunnel or a pooler that
# listens on a different port.
#
# The connection is made on the cluster's real port, but the .pgpass entry is
# written under a different (bogus) port.  Without passfileport the lookup uses
# the connection port and finds nothing; with passfileport set to the bogus
# port the lookup matches.  This test can only run with Unix-domain sockets.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (!$use_unix_sockets)
{
	plan skip_all =>
	  "authentication tests cannot run without Unix-domain sockets";
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

my $role = 'passfileport_user';
my $password = 'secret_pw';

$node->safe_psql('postgres', "CREATE ROLE $role LOGIN PASSWORD '$password'");

# Require a password (SCRAM) for this role; everything else stays trust so the
# rest of the test can keep administering the cluster.
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf', "local postgres $role scram-sha-256");
$node->append_conf('pg_hba.conf', "local all all trust");
$node->reload;

# A port that is deliberately different from the real connection port.
my $bogus_port = '1';

# Write the password file entry under the bogus port, with a wildcard host so
# the Unix-domain socket path does not have to be matched.
my $pgpassfile = $node->basedir . '/pgpass';
open(my $fh, '>', $pgpassfile) or die "could not open $pgpassfile: $!";
print $fh "*:$bogus_port:postgres:$role:$password\n";
close($fh);
chmod(0600, $pgpassfile) or die "could not chmod $pgpassfile: $!";

local $ENV{PGPASSFILE} = $pgpassfile;
delete local $ENV{PGPASSWORD};
delete local $ENV{PGPASSFILEPORT};

my $connstr = $node->connstr('postgres') . " user=$role";

# Without passfileport, the entry written under the bogus port does not match
# the real connection port, so no password is found.
$node->connect_fails(
	$connstr,
	'without passfileport the .pgpass entry under another port does not match',
	expected_stderr => qr/no password supplied/);

# With passfileport pointing at the entry's port, the lookup matches and the
# connection authenticates.
$node->connect_ok(
	"$connstr passfileport=$bogus_port",
	'passfileport makes the .pgpass lookup use the given port');

# Same behavior through the PGPASSFILEPORT environment variable.
{
	local $ENV{PGPASSFILEPORT} = $bogus_port;
	$node->connect_ok($connstr,
		'PGPASSFILEPORT environment variable makes the lookup use the given port'
	);
}

# A passfileport with no matching entry still fails.
$node->connect_fails(
	"$connstr passfileport=2",
	'passfileport with no matching .pgpass entry fails',
	expected_stderr => qr/no password supplied/);

# More passfileport values than hosts is rejected when the options are parsed.
$node->connect_fails(
	"$connstr passfileport=$bogus_port,2",
	'more passfileport values than hosts is rejected',
	expected_stderr =>
	  qr/could not match \d+ password file port numbers to \d+ hosts/);

done_testing();
