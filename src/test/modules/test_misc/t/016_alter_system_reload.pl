# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql(
	'postgres',
	q{
ALTER SYSTEM SET log_connections = 'all';
ALTER SYSTEM RELOAD;
});
ok( $node->poll_query_until(
		'postgres', q{SELECT current_setting('log_connections') = 'all'}),
	'ALTER SYSTEM RELOAD applies configuration changes');

$node->safe_psql('postgres', 'CREATE ROLE regress_alter_system_reload');
$node->safe_psql(
	'postgres',
	'GRANT ALTER SYSTEM ON PARAMETER log_connections TO regress_alter_system_reload');
my ($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	q{
SET ROLE regress_alter_system_reload;
ALTER SYSTEM SET log_connections = 'all';
});
is($ret, 0, 'parameter privilege allows ALTER SYSTEM SET');

$node->safe_psql(
	'postgres',
	'GRANT EXECUTE ON FUNCTION pg_reload_conf() TO regress_alter_system_reload');
($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	q{
SET ROLE regress_alter_system_reload;
ALTER SYSTEM RELOAD;
});
isnt($ret, 0, 'non-superuser cannot run ALTER SYSTEM RELOAD');
like(
	$stderr,
	qr/permission denied to perform ALTER SYSTEM RELOAD/,
	'permission error is reported');

($ret, $stdout, $stderr) =
  $node->psql('postgres', 'BEGIN; ALTER SYSTEM RELOAD;');
isnt($ret, 0, 'ALTER SYSTEM RELOAD is rejected in a transaction block');
like(
	$stderr,
	qr/ALTER SYSTEM cannot run inside a transaction block/,
	'transaction block error is reported');

$node->append_conf('postgresql.conf', 'allow_alter_system = off');
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');
ok( $node->poll_query_until(
		'postgres', q{SELECT current_setting('allow_alter_system') = 'off'}),
	'allow_alter_system is disabled');
($ret, $stdout, $stderr) =
  $node->psql('postgres', 'ALTER SYSTEM RELOAD;');
isnt($ret, 0, 'ALTER SYSTEM RELOAD obeys allow_alter_system');
like(
	$stderr,
	qr/ALTER SYSTEM is not allowed in this environment/,
	'allow_alter_system error is reported');

# Restore the GUC without relying on the command that it disables.
$node->append_conf('postgresql.conf', 'allow_alter_system = on');
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');
ok( $node->poll_query_until(
		'postgres', q{SELECT current_setting('allow_alter_system') = 'on'}),
	'allow_alter_system is restored');
$node->safe_psql(
	'postgres',
	q{
ALTER SYSTEM RESET log_connections;
ALTER SYSTEM RELOAD;
});

done_testing();
