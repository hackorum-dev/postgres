# Copyright (c) 2026, PostgreSQL Global Development Group

# Test recovery prefetch failure accounting.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', "full_page_writes = on");
$primary->start;

if (!$primary->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$primary->safe_psql(
	'postgres', q{
CREATE TABLE prefetch_test AS
SELECT g AS id, repeat(md5(g::text), 20) AS payload
FROM generate_series(1, 5000) AS g;
ALTER TABLE prefetch_test ADD PRIMARY KEY (id);
CHECKPOINT;
});

my $backup_name = 'backup';
$primary->backup($backup_name);

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
$standby->append_conf(
	'postgresql.conf', q{
recovery_prefetch = on
maintenance_io_concurrency = 4
});
$standby->start;

$standby->safe_psql(
	'postgres',
	q{
SELECT injection_points_attach('md-prefetch-failure', 'notice');
SELECT pg_stat_reset_shared('recovery_prefetch');
});

# The first update may generate full-page images.  The second provides
# ordinary block references that recovery can try to prefetch.
$primary->safe_psql(
	'postgres', q{
UPDATE prefetch_test
SET payload = md5(id::text || 'first') || substr(payload, 33);
UPDATE prefetch_test
SET payload = md5(id::text || 'second') || substr(payload, 33);
});
$primary->wait_for_replay_catchup($standby);

is( $standby->safe_psql(
		'postgres',
		q{
SELECT prefetch = 0 AND prefetch_failed > 0
FROM pg_stat_recovery_prefetch;
}),
	't',
	'failed advice is not counted as successful recovery prefetch');

is( $standby->safe_psql(
		'postgres',
		q{
SELECT count(*), bool_and(length(payload) > 0)
FROM prefetch_test;
}),
	'5000|t',
	'recovery completes after prefetch advice failures');

$standby->safe_psql('postgres',
	"SELECT pg_stat_reset_shared('recovery_prefetch')");
is( $standby->safe_psql(
		'postgres',
		"SELECT prefetch_failed FROM pg_stat_recovery_prefetch"),
	'0',
	'failed prefetch counter can be reset');

done_testing();
