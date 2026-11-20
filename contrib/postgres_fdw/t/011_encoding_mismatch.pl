# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Test that statistics import falls back to sampling, with a warning,
# when the remote server's encoding differs from the local one, instead
# of risking a name-matching mismatch caused by a lossy encoding
# conversion of column names.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->start;

$node->safe_psql('postgres', "CREATE EXTENSION postgres_fdw");

# Remote database with a different encoding, using non-ASCII column
# names so that a naive byte-order merge could plausibly go wrong.
$node->safe_psql(
	'postgres',
	"CREATE DATABASE remote_eucjp ENCODING 'EUC_JP' LC_COLLATE 'C' "
	  . "LC_CTYPE 'C' TEMPLATE template0");
$node->safe_psql(
	'remote_eucjp',
	"SET client_encoding = 'UTF8'; "
	  . "CREATE TABLE \"\x{6CE8}\x{6587}\" ("
	  . "\"\x{756A}\x{53F7}\" int, \"\x{9867}\x{5BA2}\" int, "
	  . "\"\x{91D1}\x{984D}\" int, \"\x{65E5}\x{4ED8}\" int); "
	  . "INSERT INTO \"\x{6CE8}\x{6587}\" "
	  . "SELECT g, g, g, g FROM generate_series(1, 1000) g; "
	  . "ANALYZE \"\x{6CE8}\x{6587}\"");

my $port = $node->port;
my $host = $node->host;
$node->safe_psql(
	'postgres', <<EOM);
SET client_encoding = 'UTF8';
CREATE SERVER eucjpsrv FOREIGN DATA WRAPPER postgres_fdw
  OPTIONS (host '$host', port '$port', dbname 'remote_eucjp');
CREATE USER MAPPING FOR CURRENT_USER SERVER eucjpsrv;
ALTER SERVER eucjpsrv OPTIONS (ADD import_stats 'true');
CREATE FOREIGN TABLE "\x{6CE8}\x{6587}" (
  "\x{756A}\x{53F7}" int, "\x{9867}\x{5BA2}" int,
  "\x{91D1}\x{984D}" int, "\x{65E5}\x{4ED8}" int)
  SERVER eucjpsrv;
EOM

my ($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	"SET client_encoding = 'UTF8'; ANALYZE \"\x{6CE8}\x{6587}\"");
like(
	$stderr,
	qr/remote server encoding "EUC_JP" differs from local encoding "UTF8"/,
	"ANALYZE falls back to sampling with a warning on encoding mismatch");

# Sanity check: a same-encoding loopback must still import normally,
# with no such warning.
$node->safe_psql(
	'postgres',
	"CREATE TABLE plain (a int, b text); "
	  . "INSERT INTO plain SELECT g, 'x' || g FROM generate_series(1, 1000) g; "
	  . "ANALYZE plain");
$node->safe_psql(
	'postgres', <<EOM);
CREATE SERVER sameenc FOREIGN DATA WRAPPER postgres_fdw
  OPTIONS (host '$host', port '$port', dbname 'postgres');
CREATE USER MAPPING FOR CURRENT_USER SERVER sameenc;
ALTER SERVER sameenc OPTIONS (ADD import_stats 'true');
CREATE FOREIGN TABLE plain_fgn (a int, b text)
  SERVER sameenc OPTIONS (table_name 'plain');
EOM

($ret, $stdout, $stderr) = $node->psql(
	'postgres', "SET client_min_messages = debug2; ANALYZE plain_fgn");
is($ret, 0, "ANALYZE of same-encoding loopback succeeds");
like(
	$stderr,
	qr/finished importing statistics for foreign table "public\.plain_fgn"/,
	"same-encoding loopback still imports statistics normally");


# A remote/local encoding mismatch with plain ASCII column names is safe
# (every server encoding we support is an ASCII-compatible superset), so
# it must still import normally, not fall back.
$node->safe_psql(
	'postgres',
	"CREATE DATABASE remote_latin1 ENCODING 'LATIN1' LC_COLLATE 'C' "
	  . "LC_CTYPE 'C' TEMPLATE template0");
$node->safe_psql(
	'remote_latin1',
	"CREATE TABLE ascii_names (id int, name text); "
	  . "INSERT INTO ascii_names SELECT g, 'x' || g FROM generate_series(1, 1000) g; "
	  . "ANALYZE ascii_names");
$node->safe_psql(
	'postgres', <<EOM);
CREATE SERVER latin1srv FOREIGN DATA WRAPPER postgres_fdw
  OPTIONS (host '$host', port '$port', dbname 'remote_latin1');
CREATE USER MAPPING FOR CURRENT_USER SERVER latin1srv;
ALTER SERVER latin1srv OPTIONS (ADD import_stats 'true');
CREATE FOREIGN TABLE ascii_names (id int, name text)
  SERVER latin1srv;
EOM

($ret, $stdout, $stderr) = $node->psql(
	'postgres', "SET client_min_messages = debug2; ANALYZE ascii_names");
is($ret, 0, "ANALYZE of ASCII-named table on mismatched-encoding loopback succeeds");
like(
	$stderr,
	qr/finished importing statistics for foreign table "public\.ascii_names"/,
	"encoding mismatch with ASCII-only names still imports statistics, not just falls back");

$node->stop;
done_testing();
