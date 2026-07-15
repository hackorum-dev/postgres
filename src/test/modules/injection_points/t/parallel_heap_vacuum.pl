# Copyright (c) 2026, PostgreSQL Global Development Group

# Tests for parallel heap vacuum. It checks that the vacuum freezes correctly
# across multiple scan rounds (with injection points forcing worker ramp-down
# and leader resume), and that it plans the expected number of workers for
# different PARALLEL requests and table sizes.

use strict;
use warnings FATAL => 'all';
use locale;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# This test requires injection points; skip it when the build lacks them.
if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;
$node->safe_psql('postgres', qq[create extension injection_points;]);

$node->safe_psql('postgres', qq[
create table t (i int) with (autovacuum_enabled = off);
create index on t (i);
		 ]);
my $nrows = 1_000_000;
my $first = int($nrows * rand());

my $psql = $node->background_psql('postgres', on_error_stop => 0);

# Hold a transaction open to keep its xmin from advancing.
$psql->query_safe('begin; select pg_current_xact_id();');

# Advance the XID a few times, each in its own transaction.
$node->safe_psql('postgres', 'select pg_current_xact_id()') for 1 .. 5;

# Insert most rows under an old XID.
$psql->query_safe(qq[insert into t select generate_series(1, $first);]);

# Insert a few rows in a newer transaction, enough to fill at least one
# page so it is not frozen by the vacuum below.
my $xid = $node->safe_psql('postgres', qq[
begin;
insert into t select 0 from generate_series(1, 300);
select pg_current_xact_id()::xid;
commit;
]);

# Insert the remaining rows and commit the open transaction.
$psql->query_safe(qq[insert into t select generate_series($first, $nrows);]);
$psql->query_safe(qq[commit;]);

# Delete a range of rows so the vacuum has dead items to collect.
$node->safe_psql('postgres', qq[delete from t where i between 1 and 20000;]);

# Run a parallel vacuum in multiple rounds. The low maintenance_work_mem
# fills the dead-item store repeatedly, and the injection points ramp the
# worker count down and disable leader participation. Everything except the
# newer transaction's rows should freeze, advancing relfrozenxid to its XID.
$node->safe_psql('postgres', qq[
set vacuum_freeze_min_age to 5;
set max_parallel_maintenance_workers TO 5;
set maintenance_work_mem TO 256;
select injection_points_set_local();
select injection_points_attach('parallel-vacuum-ramp-down-workers', 'notice');
select injection_points_attach('parallel-heap-vacuum-disable-leader-participation', 'notice');
vacuum (parallel 5, verbose) t;
		 ]);

is( $node->safe_psql('postgres', qq[select relfrozenxid from pg_class where relname = 't';]),
    "$xid", "relfrozenxid is updated as expected");

# Check if we have successfully frozen the table in the previous
# vacuum by scanning all tuples.
$node->safe_psql('postgres', qq[vacuum (freeze, parallel 0, verbose, disable_page_skipping) t;]);
is( $node->safe_psql('postgres', qq[select $xid < relfrozenxid::text::int from pg_class where relname = 't';]),
    "t", "all rows are frozen");

# Feature coverage for the number of parallel heap vacuum workers, that is, how
# many workers are planned for different PARALLEL requests, table sizes, and the
# max_parallel_maintenance_workers cap.  We assert on the "planned" count in the
# VERBOSE output, which is deterministic, rather than the launched count, which
# depends on background worker availability.

# Return the "planned" heap-vacuum worker count from a VACUUM (VERBOSE) run, or
# undef when no parallel-heap-vacuum message was emitted (i.e. serial).
sub planned_table_workers
{
	my ($sql) = @_;
	my ($ret, $stdout, $stderr) = $node->psql('postgres', $sql);
	is($ret, 0, "vacuum ran: $sql");
	if ($stderr =~ /for collecting dead tuples \(planned: (\d+)\)/)
	{
		return $1;
	}
	return undef;
}

# PARALLEL 0 disables parallel heap vacuum, so there is no message at all.
is( planned_table_workers(
		'set min_parallel_table_scan_size to "128kB"; vacuum (parallel 0, verbose) t;'
	),
	undef,
	'PARALLEL 0 launches no parallel heap vacuum workers');

# An explicit degree is honored (below the max_parallel_maintenance_workers cap).
is( planned_table_workers(
		'set min_parallel_table_scan_size to "128kB"; set max_parallel_maintenance_workers to 4; vacuum (parallel 2, verbose) t;'
	),
	2,
	'PARALLEL 2 plans 2 parallel heap vacuum workers');

# The request is capped by max_parallel_maintenance_workers.
is( planned_table_workers(
		'set min_parallel_table_scan_size to "128kB"; set max_parallel_maintenance_workers to 3; vacuum (parallel 8, verbose) t;'
	),
	3,
	'PARALLEL request is capped by max_parallel_maintenance_workers');

# A table below min_parallel_table_scan_size is vacuumed serially even when
# parallelism is requested.
$node->safe_psql('postgres', qq[
	create table small (i int) with (autovacuum_enabled = off);
	insert into small select generate_series(1, 100);
]);
is( planned_table_workers(
		'set min_parallel_table_scan_size to "1GB"; vacuum (parallel 4, verbose) small;'
	),
	undef,
	'small table is not vacuumed with parallel heap workers');

$node->stop;
done_testing();
