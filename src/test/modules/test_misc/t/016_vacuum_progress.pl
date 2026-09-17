# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Check that pg_stat_progress_vacuum reports the index being vacuumed.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (($ENV{enable_injection_points} // 'no') ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('vacprog');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql(
	'postgres', qq{
	CREATE EXTENSION injection_points;
	CREATE TABLE vacprog (a int, b int);
	INSERT INTO vacprog SELECT g, g FROM generate_series(1, 20000) g;
	CREATE INDEX vacprog_a ON vacprog (a);
	CREATE INDEX vacprog_b ON vacprog (b);
	DELETE FROM vacprog WHERE a % 2 = 0;
	SELECT injection_points_attach('vacuum-index-in-progress', 'wait');
});

# Stop inside the per index loop, with one index known to be in progress.
my $vac = $node->background_psql('postgres');
$vac->query_until(qr//, "VACUUM vacprog;\n");
$node->wait_for_event('client backend', 'vacuum-index-in-progress');

my $first = $node->safe_psql(
	'postgres', q{
	SELECT c.relname FROM pg_stat_progress_vacuum v
	  JOIN pg_class c ON c.oid = v.current_index_relid
	 WHERE v.relid = 'vacprog'::regclass});
like($first, qr/^vacprog_[ab]$/, "reports the index being vacuumed, $first");

$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('vacuum-index-in-progress');");

# The other index must be reported in its turn. A value left over from the
# first index would never satisfy this.
ok( $node->poll_query_until(
		'postgres', qq{
		SELECT count(*) = 1 FROM pg_stat_progress_vacuum v
		  JOIN pg_class c ON c.oid = v.current_index_relid
		 WHERE v.relid = 'vacprog'::regclass AND c.relname <> '$first'}),
	'reports the next index in turn');

$node->safe_psql(
	'postgres', q{
	SELECT injection_points_detach('vacuum-index-in-progress');
	SELECT injection_points_wakeup('vacuum-index-in-progress');
});
$vac->quit;
$node->stop;

done_testing();
