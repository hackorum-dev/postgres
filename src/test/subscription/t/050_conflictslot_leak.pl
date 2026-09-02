# Copyright (c) 2025-2026, PostgreSQL Global Development Group

# REPRODUCER -- NOT FOR COMMIT.
#
# Demonstrates the slot / buffer-pin leak on FindConflictTuple()'s retry path
# (src/backend/executor/execReplication.c).
#
# THE BUG
#   When table_tuple_lock() reports TM_Updated, should_refetch_tuple() sends
#   control back to the retry label.  If the next pass again finds a conflict,
#   *conflictslot is overwritten without dropping the previous slot -- and that
#   slot owns a buffer pin, because FindConflictTuple() passes no
#   TUPLE_LOCK_FLAG_FIND_LAST_VERSION, so heapam_tuple_lock() reaches
#   ExecStorePinnedBufferHeapTuple() and transfers the pin even on the
#   TM_Updated path.  (heap_lock_tuple() reaches its "failed:" label and does
#   "goto out_locked", which drops the content lock but NOT the pin.)  Each
#   retry therefore strands one more pin until the apply transaction ends.
#
# WHY THE ASSERTION IS ON A PROBE AND NOT ON pg_buffercache
#   The stranded pins are invisible to SQL.  PinBuffer() adds BUF_REFCOUNT_ONE to
#   the shared refcount only on a backend's FIRST pin of a buffer; later pins by
#   the same backend increment PrivateRefCount only.  So
#   pg_buffercache.pinning_backends -- which is BUF_STATE_GET_REFCOUNT() -- reads
#   1 whether the worker holds one pin or ten, and PrivateRefCount is not exposed
#   anywhere.  An earlier version of this test asserted on pg_buffercache and
#   passed on unpatched code for exactly that reason.
#
# THE DISCRIMINATOR: SLOT IDENTITY
#   Unpatched, each retry allocates a fresh slot and abandons the previous one.
#   Because the abandoned slot is never freed, its address cannot be reused, so
#   every pass reports a different "slot=".  With the fix a single slot is created
#   before the retry label and re-stored on each pass, so "slot=" is constant.
#   A changing address is therefore direct evidence that a slot -- still holding
#   the buffer pin heapam_tuple_lock() transferred into it -- became unreachable.
#
# SCAFFOLDING REQUIRED, in FindConflictTuple().
#
#     INJECTION_POINT("find-conflict-tuple-before-lock", NULL);
#         -- after ExecCheckIndexConstraints(), before table_tuple_lock()
#
#     elog(WARNING, "O11: pass done slot=%p buffer=%d res=%d",
#          (void *) *conflictslot,
#          ((BufferHeapTupleTableSlot *) *conflictslot)->buffer,
#          (int) res);
#         -- between PopActiveSnapshot() and should_refetch_tuple()
#
#   The injection point is unavoidable: the window is between
#   FindConflictTuple()'s own ExecCheckIndexConstraints() and its
#   table_tuple_lock(), and nothing reachable from SQL can be timed into it.
#   (An uncommitted UPDATE does not work -- the worker blocks earlier, in the
#   unique-index check inside ExecSimpleRelationInsert().)
#
# EXPECTED RESULTS -- same test, same scaffolding, both ways:
#   unpatched -> $cycles + 1 distinct slot addresses: FAILS
#   fixed     -> exactly 1 distinct slot address:     PASSES
#
# Run with:
#   make -C src/test/subscription check PROVE_TESTS='t/zz_o11_conflictslot_leak.pl'

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Retry cycles to force.  Each one strands a further pin on unpatched code.
my $cycles = 3;

###############################
# Setup
###############################

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

# The injection point is reached by the apply worker, i.e. on the subscriber.
# No shared_preload_libraries needed -- injection_wait() backs its state with
# GetNamedDSMSegment(), so 'wait' mode works with just CREATE EXTENSION.
if ($node_subscriber->check_extension('injection_points') == 0)
{
	plan skip_all =>
	  'this test requires the injection_points module (build with --enable-injection-points)';
}

$node_publisher->safe_psql('postgres',
	"CREATE TABLE conf_tab (a int PRIMARY KEY, b text);");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE conf_tab (a int PRIMARY KEY, b text);");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub FOR TABLE conf_tab;");

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub CONNECTION '$publisher_connstr' PUBLICATION pub;"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'sub');

$node_subscriber->safe_psql('postgres', "CREATE EXTENSION injection_points;");

###############################
# Force $cycles retries inside FindConflictTuple()
###############################

$node_subscriber->safe_psql('postgres',
	"INSERT INTO conf_tab VALUES (1, 'local');");

$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('find-conflict-tuple-before-lock', 'wait');"
);

my $log_location = -s $node_subscriber->logfile;

# Replicates and violates conf_tab's primary key on the subscriber.
$node_publisher->safe_psql('postgres',
	"INSERT INTO conf_tab VALUES (1, 'remote');");

for my $i (1 .. $cycles)
{
	# Parked at pass $i: conflict found, tuple not yet locked.
	$node_subscriber->wait_for_event('logical replication apply worker',
		'find-conflict-tuple-before-lock');

	# Update and commit the conflicting row while the worker is parked, so the
	# pending table_tuple_lock() reports TM_Updated rather than TM_Ok.
	$node_subscriber->safe_psql('postgres',
		"UPDATE conf_tab SET b = 'changed$i' WHERE a = 1;");

	my $loc = -s $node_subscriber->logfile;

	$node_subscriber->safe_psql('postgres',
		"SELECT injection_points_wakeup('find-conflict-tuple-before-lock');");

	# Confirm this cycle's lock really failed and the retry was taken.
	$node_subscriber->wait_for_log(qr/concurrent update, retrying/, $loc);
}

# Release the final pass and disarm, so the worker can finish and its
# subsequent retries of the failed transaction do not park again.
$node_subscriber->wait_for_event('logical replication apply worker',
	'find-conflict-tuple-before-lock');
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_wakeup('find-conflict-tuple-before-lock');");
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_detach('find-conflict-tuple-before-lock');");

$node_subscriber->wait_for_log(qr/conflict detected on relation/, $log_location);

###############################
# Verdict
###############################

my $log = slurp_file($node_subscriber->logfile, $log_location);

# Scope to the single controlled apply attempt; the worker retries the
# transaction afterwards, since insert_exists is raised at ERROR.
my ($attempt) = ($log =~ /^(.*?conflict detected on relation)/s);
$attempt //= $log;

my @passes = ($attempt =~ /O11: pass done slot=(0x[0-9a-f]+) buffer=\d+ res=(\d+)/g);

if (!@passes)
{
	BAIL_OUT(
		"no 'O11: pass done' messages found -- the diagnostic scaffolding is not\n"
		  . "present in this build.  Add the probe to FindConflictTuple(), rebuild\n"
		  . "the backend, reinstall, and re-run.");
}

# Pairs of (slot address, TM_Result).
my (@slots, @results);
while (@passes)
{
	push @slots,   shift @passes;
	push @results, shift @passes;
}

# TM_Updated is 3.  Every cycle but the last must have failed that way, proving
# the retry path was genuinely exercised in this build.
my $tm_updated = grep { $_ == 3 } @results;
is($tm_updated, $cycles,
	"forced $cycles table_tuple_lock() failures through the TM_Updated path");

# THE ASSERTION.  One slot must serve every pass.  Unpatched, each retry
# allocates a fresh slot and abandons the previous one -- and since the
# abandoned slot is never freed, its address cannot be reused, so the addresses
# differ.  A distinct count above one therefore means slots were stranded, each
# still holding the buffer pin heapam_tuple_lock() transferred into it.
my %seen;
$seen{$_} = 1 for @slots;
my $distinct = scalar keys %seen;

is($distinct, 1,
	"a single conflict slot served all " . scalar(@slots) . " passes (none stranded)"
);

if ($distinct != 1)
{
	diag(
		"REPRODUCED: $distinct distinct conflict slots were used across "
		  . scalar(@slots)
		  . " passes:\n  "
		  . join("\n  ", @slots)
		  . "\n"
		  . "Each pass after the first overwrote *conflictslot without dropping the\n"
		  . "previous slot, stranding that slot's transferred buffer pin until the\n"
		  . "apply transaction ends.  (The pin depth itself is invisible to\n"
		  . "pg_buffercache: PinBuffer() bumps the shared refcount only on a\n"
		  . "backend's first pin of a buffer.)  Apply the O11 fix and re-run.");
}

###############################
# Cleanup: let replication converge so the subscription is not left erroring.
###############################

$node_subscriber->safe_psql('postgres', "DELETE FROM conf_tab WHERE a = 1;");
$node_publisher->wait_for_catchup('sub');

is( $node_subscriber->safe_psql(
		'postgres', "SELECT a, b FROM conf_tab ORDER BY a;"),
	qq(1|remote),
	'replication converges once the conflicting local row is removed');

done_testing();
