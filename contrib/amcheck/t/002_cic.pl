
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Test CREATE INDEX CONCURRENTLY with concurrent modifications
use strict;
use warnings;

use Config;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More tests => 3;

my ($node, $result);

#
# Test set-up
#
$node = PostgreSQL::Test::Cluster->new('CIC_test');
$node->init;
$node->append_conf('postgresql.conf', 'lock_timeout = 180000');
$node->start;
$node->safe_psql('postgres', q(CREATE EXTENSION amcheck));
$node->safe_psql('postgres', q(CREATE TABLE tbl(k int, v int)));
$node->safe_psql('postgres', q(INSERT INTO tbl SELECT n, 0 FROM generate_series(0,9) t(n)));
$node->safe_psql('postgres', q(CREATE INDEX idx ON tbl(k)));

#
# Stress CIC with pgbench.
#
# pgbench might try to launch more than one instance of the CIC
# transaction concurrently.  That would deadlock, so use an advisory
# lock to ensure only one CIC runs at a time.
#
$node->pgbench(
	'--no-vacuum --client=5 --transactions=20000',
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent INSERTs and CIC',
	{
		'hot_update_commit' => q(
			UPDATE tbl SET v = v + 1 WHERE k = :client_id;
		  ),
		'hot_update_rollback' => q(
			BEGIN;
			UPDATE tbl SET v = v + 1 WHERE k = :client_id;
			ROLLBACK;
		  ),
		'vacuum' => q(
			VACUUM FREEZE tbl;
		  ),
		'key_share' => q(
			SELECT * FROM tbl FOR KEY SHARE;
		  ),
		'002_pgbench_concurrent_cic' => q(
			SELECT pg_try_advisory_lock(42)::integer AS gotlock \gset
			\if :gotlock
				REINDEX INDEX CONCURRENTLY idx;
				SELECT bt_index_check('idx',true);
				SELECT pg_advisory_unlock(42);
			\endif
		  )
	});

print $node->safe_psql('postgres',
					   q(SELECT * FROM pg_stat_all_tables where relname = 'tbl'),
					   extra_params => ['--expanded']);

$node->stop;
done_testing();
