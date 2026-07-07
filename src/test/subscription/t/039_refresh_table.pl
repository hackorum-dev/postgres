
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Tests for ALTER SUBSCRIPTION ... REFRESH TABLE, which re-copies one or more
# already-subscribed tables on the subscriber without touching publication
# membership or other tables.  The first version requires the subscription
# to be disabled.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize publisher and subscriber nodes
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->append_conf('postgresql.conf',
	"wal_retrieve_retry_interval = 1ms");
$node_subscriber->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

# Preexisting content on the publisher: two published tables.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_res   (a int primary key, b text);
	CREATE TABLE tab_other (a int primary key, b text);
	INSERT INTO tab_res   SELECT g, 'p' || g FROM generate_series(1, 100) g;
	INSERT INTO tab_other SELECT g, 'q' || g FROM generate_series(1, 100) g;
	CREATE PUBLICATION tap_pub FOR TABLE tab_res, tab_other;
));

# Matching structure on the subscriber, plus objects used for error cases.
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_res   (a int primary key, b text);
	CREATE TABLE tab_other (a int primary key, b text);
	CREATE TABLE tab_local (a int primary key);
	CREATE SEQUENCE seq_local;
));

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub"
);

# Wait for initial sync of both tables.
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_res"),
	'100', 'initial sync of tab_res');
is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_other"),
	'100', 'initial sync of tab_other');

# A small helper: run SQL expected to fail, and check the error message.
sub refresh_should_fail
{
	my ($sql, $pattern, $desc) = @_;
	my ($ret, $stdout, $stderr) = ('', '', '');
	$ret = $node_subscriber->psql('postgres', $sql,
		stdout => \$stdout, stderr => \$stderr);
	ok($ret != 0 && $stderr =~ /$pattern/,
		$desc)
	  or diag("got ret=$ret stderr=$stderr");
}

# REFRESH TABLE is not allowed while the subscription is enabled.
refresh_should_fail(
	"ALTER SUBSCRIPTION tap_sub REFRESH TABLE tab_res",
	qr/not allowed for enabled subscriptions/,
	'REFRESH TABLE rejected while enabled');

# Disable the subscription and wait for its workers to stop.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub DISABLE");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_subscription WHERE subname = 'tap_sub' AND pid IS NOT NULL"
) or die "Timed out waiting for subscription workers to stop";

# A table that is not part of the subscription is rejected.
refresh_should_fail(
	"ALTER SUBSCRIPTION tap_sub REFRESH TABLE tab_local",
	qr/is not part of the subscription/,
	'REFRESH TABLE rejected for table not in subscription');

# A sequence cannot be refreshed as a table.
refresh_should_fail(
	"ALTER SUBSCRIPTION tap_sub REFRESH TABLE seq_local",
	qr/cannot refresh sequence/,
	'REFRESH TABLE rejected for a sequence');

# The command cannot run inside a transaction block.
refresh_should_fail(
	"BEGIN; ALTER SUBSCRIPTION tap_sub REFRESH TABLE tab_res;",
	qr/cannot run inside a transaction block/,
	'REFRESH TABLE rejected inside a transaction block');

# All-or-nothing: a list containing one bad table aborts the whole command and
# leaves the valid table untouched.
refresh_should_fail(
	"ALTER SUBSCRIPTION tap_sub REFRESH TABLE tab_res, tab_local",
	qr/is not part of the subscription/,
	'REFRESH TABLE with a bad table in the list is rejected');
is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT srsubstate FROM pg_subscription_rel r JOIN pg_class c ON c.oid = r.srrelid WHERE c.relname = 'tab_res'"
	),
	'r',
	'valid table not reset when another table in the list is invalid');

# Introduce drift on the subscriber, only in tab_res.
$node_subscriber->safe_psql(
	'postgres', qq(
	DELETE FROM tab_res WHERE a <= 40;
	UPDATE tab_res SET b = 'CORRUPT' WHERE a = 60;
));
is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_res"),
	'60', 'drift introduced in tab_res');

# Record the pre-refresh state of the other table.
my $other_state_before = $node_subscriber->safe_psql('postgres',
	"SELECT srsubstate FROM pg_subscription_rel r JOIN pg_class c ON c.oid = r.srrelid WHERE c.relname = 'tab_other'"
);
is($other_state_before, 'r', 'tab_other is ready before refresh');

# Resync just tab_res.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub REFRESH TABLE tab_res");

# Only tab_res is reset to init; tab_other is untouched; local copy truncated.
is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT srsubstate FROM pg_subscription_rel r JOIN pg_class c ON c.oid = r.srrelid WHERE c.relname = 'tab_res'"
	),
	'i',
	'tab_res reset to init state');
is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT srsubstate FROM pg_subscription_rel r JOIN pg_class c ON c.oid = r.srrelid WHERE c.relname = 'tab_other'"
	),
	'r',
	'tab_other left untouched (still ready)');
is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_res"),
	'0', 'tab_res truncated locally by REFRESH while disabled');

# Re-enable and wait for the single table to re-copy.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub ENABLE");
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_res"),
	'100', 'tab_res re-copied after enable');
is( $node_subscriber->safe_psql('postgres',
		"SELECT b FROM tab_res WHERE a = 60"),
	'p60', 'tab_res corruption repaired by resync');

# Full content matches the publisher.
my $pub_md5 = $node_publisher->safe_psql('postgres',
	"SELECT md5(string_agg(a || ':' || b, ',' ORDER BY a)) FROM tab_res");
my $sub_md5 = $node_subscriber->safe_psql('postgres',
	"SELECT md5(string_agg(a || ':' || b, ',' ORDER BY a)) FROM tab_res");
is($sub_md5, $pub_md5, 'tab_res matches publisher after resync');

# tab_other was never disturbed.
is( $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_other"),
	'100', 'tab_other intact throughout');

# Ongoing replication still works for the resynced table.
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_res VALUES (101, 'p101')");
$node_publisher->wait_for_catchup('tap_sub');
is( $node_subscriber->safe_psql('postgres',
		"SELECT count(*) FROM tab_res WHERE a = 101"),
	'1', 'streaming resumes on resynced table');

# Multiple tables can be resynced in a single command.  Disable, drift both
# tables, and refresh them together.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub DISABLE");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_subscription WHERE subname = 'tap_sub' AND pid IS NOT NULL"
) or die "Timed out waiting for subscription workers to stop";

$node_subscriber->safe_psql(
	'postgres', qq(
	DELETE FROM tab_res   WHERE a <= 20;
	DELETE FROM tab_other WHERE a <= 20;
));

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub REFRESH TABLE tab_res, tab_other");

# Both listed tables are reset to init and truncated locally.
is( $node_subscriber->safe_psql(
		'postgres',
		"SELECT string_agg(srsubstate, ',' ORDER BY c.relname) FROM pg_subscription_rel r JOIN pg_class c ON c.oid = r.srrelid WHERE c.relname IN ('tab_other', 'tab_res')"
	),
	'i,i',
	'both listed tables reset to init state');
is( $node_subscriber->safe_psql('postgres',
		"SELECT (SELECT count(*) FROM tab_res) + (SELECT count(*) FROM tab_other)"),
	'0', 'both listed tables truncated locally');

# Re-enable and wait for both tables to re-copy.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub ENABLE");
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

my $pub_res = $node_publisher->safe_psql('postgres',
	"SELECT md5(string_agg(a || ':' || b, ',' ORDER BY a)) FROM tab_res");
my $sub_res = $node_subscriber->safe_psql('postgres',
	"SELECT md5(string_agg(a || ':' || b, ',' ORDER BY a)) FROM tab_res");
is($sub_res, $pub_res, 'tab_res matches publisher after multi-table resync');

my $pub_oth = $node_publisher->safe_psql('postgres',
	"SELECT md5(string_agg(a || ':' || b, ',' ORDER BY a)) FROM tab_other");
my $sub_oth = $node_subscriber->safe_psql('postgres',
	"SELECT md5(string_agg(a || ':' || b, ',' ORDER BY a)) FROM tab_other");
is($sub_oth, $pub_oth, 'tab_other matches publisher after multi-table resync');

$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub");
$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
