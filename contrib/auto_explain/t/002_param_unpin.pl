# Copyright (c) 2026, PostgreSQL Global Development Group

# A lazy SQL-language SRF is shut down from the caller's ExecutorEnd, after
# that plan has dropped its buffer pins. auto_explain then formats the
# function arguments. This test stops at that point, replaces the unpinned
# page, and checks that the logged parameter is still the original value.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

plan skip_all => 'Injection points not supported by this build'
  unless $ENV{enable_injection_points} eq 'yes';

my $node = PostgreSQL::Test::Cluster->new('main');

$node->init;
$node->append_conf(
	'postgresql.conf', q{
shared_buffers = 1MB
shared_preload_libraries = 'auto_explain,injection_points'
auto_explain.log_min_duration = 0
auto_explain.log_nested_statements = on
auto_explain.log_parameter_max_length = -1
log_min_messages = log
});
$node->start;

plan skip_all => 'Extension injection_points not installed'
  unless $node->check_extension('injection_points');
plan skip_all => 'Extension pg_buffercache not installed'
  unless $node->check_extension('pg_buffercache');

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION pg_buffercache;

CREATE TABLE t (v text);
INSERT INTO t SELECT repeat('abc', 10) FROM generate_series(1, 100);
CREATE FUNCTION srf(a text) RETURNS SETOF int LANGUAGE sql
  AS $$ SELECT g FROM generate_series(1, 10) g $$;
});

my $point = 'auto-explain-before-params';

$node->safe_psql('postgres',
	"SELECT injection_points_attach('$point', 'wait');");

my $victim = $node->background_psql('postgres', on_error_stop => 0);

$victim->query_until(
	qr/starting_select/, q{
\echo starting_select
SELECT srf(v) FROM t LIMIT 1;
});

ok(wait_for_injection_point($node, $point), "hit injection point $point");

# Evict leaves the old bytes in place. A second relation has to reuse the page.
$node->safe_psql(
	'postgres', q{
SELECT buffers_evicted FROM pg_buffercache_evict_all();
CREATE TABLE filler (v text);
INSERT INTO filler SELECT repeat('Z', 2000) FROM generate_series(1, 800);
SELECT count(*) FROM filler;
});

$node->safe_psql(
	'postgres', qq{
SELECT injection_points_detach('$point');
SELECT injection_points_wakeup('$point');
});

my ($out, $failed) = $victim->query('\\echo done_select');
is($failed, 0, 'query did not error')
  or diag($victim->{stderr});
like($out, qr/\b1\b/, 'query returned a row');

my $log = slurp_file($node->logfile);
like(
	$log,
	qr/Query Parameters: \$1 = 'abcabcabcabcabcabcabcabcabcabc'/,
	'logged parameter is the original value');

$victim->quit;
$node->stop;

done_testing();

sub wait_for_injection_point
{
	my ($node, $point_name, $timeout) = @_;
	$timeout //= $PostgreSQL::Test::Utils::timeout_default / 2;

	for (my $elapsed = 0; $elapsed < $timeout * 10; $elapsed++)
	{
		my $pid = $node->safe_psql(
			'postgres', qq{
			SELECT pid FROM pg_stat_activity
			WHERE wait_event_type = 'InjectionPoint'
			  AND wait_event = '$point_name'
			LIMIT 1;
		});
		return 1 if $pid ne '';
		usleep(100_000);
	}
	return 0;
}
