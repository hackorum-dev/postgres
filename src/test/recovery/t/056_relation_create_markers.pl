# Copyright (c) 2026, PostgreSQL Global Development Group

# Test cleanup of permanent relation files created by transactions that are
# still in progress when the server crashes.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('relation_create_markers');
$node->init();
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 10');
$node->start();

my $marker_dir = $node->data_dir . '/pg_relcreate';

$node->safe_psql('postgres', 'CREATE TABLE committed_relation (a int)');
is(scalar(grep { $_ ne '.' && $_ ne '..' } slurp_dir($marker_dir)),
	0, 'committed relation leaves no creation marker');

my $session = $node->background_psql('postgres');
$session->query_safe('BEGIN');
my $relation_path = $session->query_safe(
	'CREATE TABLE crash_aborted_relation (a int); '
	  . q{SELECT pg_relation_filepath('crash_aborted_relation')});

ok(-f $node->data_dir . '/' . $relation_path,
	'uncommitted relation file exists before crash');
is(scalar(grep { $_ ne '.' && $_ ne '..' } slurp_dir($marker_dir)),
	1, 'uncommitted relation has a creation marker');

# Move the redo pointer past the creation record.  Recovery therefore needs
# the persistent marker; replay-local tracking of the create record is not
# sufficient.
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$node->start();

is($node->safe_psql('postgres',
	q{SELECT to_regclass('crash_aborted_relation') IS NULL}),
	't', 'crash-aborted relation is absent from the catalog');
ok(!-e $node->data_dir . '/' . $relation_path,
	'crash-aborted relation file is removed during recovery');
is(scalar(grep { $_ ne '.' && $_ ne '..' } slurp_dir($marker_dir)),
	0, 'processed creation marker is removed');

my $prepared_path = $node->safe_psql(
	'postgres',
	q{BEGIN;
CREATE TABLE prepared_relation (a int);
SELECT pg_relation_filepath('prepared_relation');
PREPARE TRANSACTION 'relation_create_marker';});
ok(-f $node->data_dir . '/' . $prepared_path,
	'prepared relation file exists');
is(scalar(grep { $_ ne '.' && $_ ne '..' } slurp_dir($marker_dir)),
	1, 'prepared relation retains its creation marker');

$node->stop('immediate');
$node->start();

ok(-f $node->data_dir . '/' . $prepared_path,
	'prepared relation file survives recovery');
is(scalar(grep { $_ ne '.' && $_ ne '..' } slurp_dir($marker_dir)),
	1, 'recovery retains prepared relation marker');
$node->safe_psql('postgres',
	q{COMMIT PREPARED 'relation_create_marker'});
is($node->safe_psql('postgres',
	q{SELECT to_regclass('prepared_relation') IS NOT NULL}),
	't', 'committed prepared relation is visible');
is(scalar(grep { $_ ne '.' && $_ ne '..' } slurp_dir($marker_dir)),
	0, 'commit prepared removes relation marker');

$node->stop();
done_testing();
