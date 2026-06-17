# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Reproducer for a hot-standby index-only-scan (IOS) correctness bug.
#
# Concurrent heap TID recycling is normally interlocked by the btree leaf-page
# buffer pin: VACUUM must take a cleanup lock on *every* leaf page before it can
# recycle any heap TID (src/backend/access/nbtree/README, "Making concurrent TID
# recycling safe").  An IOS holds the pin on the leaf page whose index tuples it
# has buffered, so VACUUM cannot recycle those TIDs until the scan is done -- the
# heap LP_DEAD tombstones stay put, and the IOS never trusts an all-visible VM
# bit for a TID whose heap slot was recycled out from under it.
#
# During recovery the standby does NOT take a cleanup lock on every leaf page; it
# only takes one when replaying an XLOG_BTREE_VACUUM record, which the primary
# emits only for leaf pages it actually deletes index tuples from (nbtree.c,
# _bt_delitems_vacuum).  So whenever the dead index entries an IOS has buffered
# leave its pinned page WITHOUT an XLOG_BTREE_VACUUM on that page, the standby
# never cleanup-locks the pinned page, nothing blocks replay of the heap
# recycling, and the IOS returns its locally-buffered stale index keys as live
# rows: phantoms.
#
# The hazard is physical, not MVCC.  By the time VACUUM recycles the tombstones
# the tuples are dead to every snapshot, so neither recovery conflicts nor
# hot_standby_feedback can prevent it -- both are MVCC-shaped, and this is about
# a buffered *physical TID reference*, not visibility.
#
# Every scenario buffers the same picture on the cursor's pinned leaf P: a live
# anchor a=1 plus a large group of dead-to-all duplicate entries a=PHANTOM.  They
# differ only in how those dead entries leave P:
#
#   'split'    BUG: inserting more duplicates splits P, moving the whole dead
#                   group to a right sibling.  VACUUM's XLOG_BTREE_VACUUM lands on
#                   the sibling, never on P (README's "items moved right" case).
#   'simpledel' BUG: a plain index scan marks the dead entries LP_DEAD and an
#                   insert reclaims them in place by simple deletion, replayed as
#                   XLOG_BTREE_DELETE under a plain exclusive lock, not a cleanup
#                   lock (nbtxlog.c btree_xlog_delete).
#   'onpage'   CONTROL: the dead entries stay on P; VACUUM removes them there and
#                   emits XLOG_BTREE_VACUUM for P, so replay cleanup-locks P and a
#                   buffer-pin conflict cancels the cursor (the interlock working).
#
# The two BUG routes check that the standby index-only scan returns no phantom
# rows, and on failure print the rows it wrongly returned (index entries for
# recycled heap TIDs, visible only because of the all-visible bit).  The 'onpage'
# CONTROL shows the safe outcome -- the cursor is cancelled -- so a BUG-route
# failure is a real defect, not a setup artifact.  Each BUG route runs with
# aborted phantoms; 'simpledel' also runs with a committed DELETE, to show the
# aborted xact is a convenience, not the cause.
#
# A duplicate group is what makes the 'split' route deterministic: the split code
# refuses to divide a group of equal keys, so with a lone distinct anchor below a
# big duplicate group the split point can only fall between them -- the anchor
# stays alone on P (with a high key, which points to no heap tuple and is never
# recycled) and the entire dead group moves right, regardless of the exact split
# arithmetic (nbtsplitloc.c, SPLIT_MANY_DUPLICATES).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# ---------------------------------------------------------------------------
# Cluster: primary + streaming hot standby.
#
# We steer the primary's OldestXmin with hot_standby_feedback -- a *standby*-side
# xmin.  (A snapshot held open on the primary would instead stop VACUUM's recycle
# pass from ever marking the emptied phantom blocks all-visible, so feedback is
# the route that works.)  wal_receiver_status_interval is tiny so the feedback
# xmin tracks the standby closely; the default ~10s is stale enough that the
# anchor row would never get marked all-visible in time.
# ---------------------------------------------------------------------------
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf(
	'postgresql.conf', qq[
autovacuum = off
]);
$node_primary->start;
$node_primary->safe_psql('postgres',
	'CREATE EXTENSION pageinspect; CREATE EXTENSION pg_walinspect;');
$node_primary->backup('b');

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, 'b', has_streaming => 1);
$node_standby->append_conf(
	'postgresql.conf', qq[
hot_standby = on
hot_standby_feedback = on
wal_receiver_status_interval = 1
max_standby_streaming_delay = 100ms
]);
$node_standby->start;

# The phantom value, the size of its duplicate group buffered on P, and the extra
# duplicates inserted to trigger the 'split' route.  ANCHOR + NDUP entries fit on
# one leaf (so FETCH 1 buffers the whole group); + NTRIG overflows it, forcing a
# split.  fillfactor=10 + a wide pad gives one heap row per block, so each
# recycled row leaves an *empty* block that VACUUM marks all-visible+all-frozen.
my $PHANTOM = 1000;
my $NDUP = 350;
my $NTRIG = 120;
my $PRESS = 100;  # distinct low keys that pressure P in the 'simpledel' route

# The leftmost leaf has no left sibling (btpo_prev = 0); a forward scan from the
# smallest key lands there, so it is the page the IOS cursor pins.  Use the true
# on-disk page count, not the (stale between vacuums) pg_class.relpages.
sub leftmost_leaf
{
	my ($idx) = @_;
	return $node_primary->safe_psql(
		'postgres', qq[
		SELECT s.blkno
		FROM generate_series(1, pg_relation_size('$idx')/current_setting('block_size')::int - 1) AS g,
		     LATERAL bt_page_stats('$idx', g::int) AS s
		WHERE s.type = 'l' AND s.btpo_prev = 0
		ORDER BY s.blkno LIMIT 1]);
}

# Create the dead-to-all duplicate group a=PHANTOM on the (single) leaf P.
#   'abort'  -> a ROLLED-BACK insert.  Aborted heap tuples are dead to every
#               snapshot unconditionally, so VACUUM recycles them whatever
#               OldestXmin is, and deleting/recycling them carries an Invalid
#               conflict horizon (no snapshot conflict).
#   'delete' -> ordinary rows removed by a committed DELETE (returns its xid):
#               the proof that the aborted xact is just a convenience.  Recycling
#               the tombstones needs OldestXmin past the deleting xid, achieved by
#               making the cursor's (feedback-reported) xmin greater than it.
sub make_dead_dups
{
	my ($tbl, $n, $mode) = @_;
	if ($mode eq 'abort')
	{
		$node_primary->safe_psql(
			'postgres', qq[
			BEGIN;
			INSERT INTO $tbl SELECT $PHANTOM FROM generate_series(1, $n) g;
			ROLLBACK;
		]);
		return 0;
	}
	$node_primary->safe_psql('postgres',
		"INSERT INTO $tbl SELECT $PHANTOM FROM generate_series(1, $n) g;");
	my $d = $node_primary->safe_psql('postgres',
		"BEGIN; DELETE FROM $tbl WHERE a = $PHANTOM; SELECT txid_current(); COMMIT;"
	);
	chomp $d;
	return $d;
}

# Run one scenario on its own table.  $route is how the buffered dead entries
# leave P ('split'/'simpledel' = BUG, 'onpage' = CONTROL); $mode is how they were
# made dead ('abort'/'delete').  Returns a hashref describing the outcome.
sub run_scenario
{
	my ($tbl, $route, $mode) = @_;
	my $idx = "${tbl}_a";

	# a=1 is a committed live anchor for the cursor to rest on.  Crucially we
	# VACUUM it all-visible *before* the phantom rows exist, so the cursor can
	# return a=1 from the VM at FETCH 1 without a heap fetch.  An anchor heap fetch
	# would pin the anchor's heap page, which the recycle VACUUM later cleanup-locks
	# (to set it all-visible), cancelling the cursor with an unrelated buffer-pin
	# conflict.
	#
	# Marking the anchor all-visible needs OldestXmin past a=1's xmin.  OldestXmin
	# here is the feedback xmin, so wait for feedback to advance past the anchor
	# first -- without this the VACUUM leaves the anchor not-all-visible and the bug
	# does not reproduce (the cursor heap-fetches a=1 and is cancelled).
	$node_primary->safe_psql(
		'postgres', qq[
		CREATE TABLE $tbl (a int NOT NULL, pad char(1024) DEFAULT '')
		  WITH (autovacuum_enabled = false, fillfactor = 10);
		INSERT INTO $tbl VALUES (1);
		-- Disable deduplication so each duplicate is its own index tuple (no
		-- posting lists): the split code sees a real group to keep together, and
		-- the insert-time reclaim path can only pick simple deletion.
		CREATE INDEX $idx ON $tbl (a) WITH (deduplicate_items = off);
	]);
	my $a1xmin = $node_primary->safe_psql('postgres',
		"SELECT xmin::text::bigint FROM $tbl WHERE a = 1");
	chomp $a1xmin;
	$node_primary->poll_query_until(
		'postgres', qq[
		SELECT backend_xmin IS NOT NULL AND backend_xmin::text::bigint > $a1xmin
		FROM pg_stat_replication]
	) or die "$tbl: feedback never advanced past anchor xmin";
	$node_primary->safe_psql('postgres', "VACUUM (TRUNCATE false) $tbl;");
	$node_primary->wait_for_replay_catchup($node_standby);

	# Build the dead duplicate group on P.
	my $D = make_dead_dups($tbl, $NDUP, $mode);
	$node_primary->wait_for_replay_catchup($node_standby);

	# Precondition: anchor + the whole dead group still sit on ONE leaf, so FETCH 1
	# buffers the entire group.  (A premature split here would silently leave most
	# of the group off P and unbuffered.)
	my $nblocks = $node_primary->safe_psql('postgres',
		"SELECT pg_relation_size('$idx')/current_setting('block_size')::int;"
	);
	is($nblocks, '2',
		"$tbl: anchor + dead group buffered on a single leaf P");

	my $P = leftmost_leaf($idx);
	note "$tbl: leftmost leaf (cursor pins this) = block $P"
	  . ($mode eq 'delete' ? "; deleting xid D=$D" : "");

	# Confirm an Index-ONLY Scan on the standby (a plain Index Scan would visit the
	# heap and not exhibit the bug).
	my $plan = $node_standby->safe_psql(
		'postgres', qq[
		SET enable_seqscan=off; SET enable_bitmapscan=off;
		EXPLAIN (COSTS OFF) SELECT a FROM $tbl WHERE a >= 1;]);
	like(
		$plan,
		qr/Index Only Scan/,
		"$tbl: cursor query uses an index-only scan");

	# Open the IOS cursor and FETCH 1: reads the whole leaf P into the scan's local
	# buffer (so->currTuples) -- including the dead a=PHANTOM entries, still
	# physically present -- returns the anchor a=1 from the VM, and holds the buffer
	# pin on P.
	my $sb = $node_standby->background_psql('postgres', on_error_stop => 0);
	my $f1 = $sb->query_safe(
		qq[
		BEGIN ISOLATION LEVEL REPEATABLE READ;
		SET enable_seqscan=off; SET enable_bitmapscan=off;
		DECLARE c NO SCROLL CURSOR FOR SELECT a FROM $tbl WHERE a >= 1;
		FETCH 1 FROM c;]);
	like($f1, qr/^1$/m,
		"$tbl: FETCH 1 returned anchor a=1 and pinned leaf P");

	# The cursor's xmin will, via feedback, hold the primary's OldestXmin at it.
	# For 'delete' that must be past D so the recycle actually removes the
	# tombstones (and so the recycle record's conflict horizon = D < cursor xmin,
	# no snapshot conflict).  Wait for feedback to fully catch up to the cursor's
	# xmin before the recycle VACUUM.
	my $cur_xmin = $sb->query_safe(
		'SELECT backend_xmin::text::bigint FROM pg_stat_activity WHERE pid = pg_backend_pid();'
	);
	chomp $cur_xmin;
	cmp_ok($cur_xmin, '>', $D,
		"$tbl: cursor snapshot xmin ($cur_xmin) is past the deleting xid D ($D)"
	) if $mode eq 'delete';
	$node_primary->poll_query_until(
		'postgres', qq[
		SELECT backend_xmin IS NOT NULL AND backend_xmin::text::bigint >= $cur_xmin
		FROM pg_stat_replication]
	) or die "$tbl: feedback never caught up to cursor xmin";

	my $logpos = -s $node_standby->logfile;
	my $wal_start =
	  $node_primary->safe_psql('postgres', 'SELECT pg_current_wal_lsn();');

	# Take the buffered dead entries off P (or, for the CONTROL, leave them on P).
	if ($route eq 'split')
	{
		# Insert more dead duplicates.  The page overflows and splits; because the
		# split code will not divide the equal-key group, the lone anchor stays on
		# P (with a high key only) and the entire dead group -- the buffered entries
		# included -- moves to a right sibling.  No index entry is removed from P.
		make_dead_dups($tbl, $NTRIG, $mode);
	}
	elsif ($route eq 'simpledel')
	{
		# Remove the dead entries from P *in place*, without a VACUUM btree record:
		#  (1) a plain (non-IOS) index scan visits the heap, finds the tuples dead,
		#      and marks the matching index entries LP_DEAD on P;
		#  (2) inserting distinct low keys pressures P; with LP_DEAD items present
		#      and no room, the insert runs simple deletion -> XLOG_BTREE_DELETE on
		#      P (exclusive lock on replay, no cleanup lock).
		# The pressure keys (2..) differ from the phantom value, so they never
		# collide with it in the ground-truth check.
		$node_primary->safe_psql(
			'postgres', qq[
			SET enable_indexonlyscan=off; SET enable_seqscan=off; SET enable_bitmapscan=off;
			SELECT count(*) FROM $tbl WHERE a = $PHANTOM;
		]);
		$node_primary->safe_psql('postgres',
			"INSERT INTO $tbl SELECT g FROM generate_series(2, @{[ 1 + $PRESS ]}) g;"
		);
	}

	# Recycle the dead heap tuples.  INDEX_CLEANUP on forces the index-vacuuming
	# pass and hence the heap second pass that turns LP_DEAD into LP_UNUSED and
	# marks the emptied blocks all-visible.  Plain VACUUM, and no CHECKPOINT
	# anywhere in the scenario: a FREEZE, or an FPI emitted within VACUUM after a
	# checkpoint, makes the recycle freeze opportunistically and emit records whose
	# conflict horizon cancels the cursor.
	$node_primary->safe_psql('postgres',
		"VACUUM (TRUNCATE false, INDEX_CLEANUP on) $tbl;");
	$node_primary->wait_for_replay_catchup($node_standby);

	# Did any XLOG_BTREE_VACUUM (cleanup-lock-on-replay) record land on the pinned
	# page P?  (pg_walinspect on the primary.)  Both BUG routes must emit none;
	# the CONTROL must emit at least one.
	my $vacuum_on_P = $node_primary->safe_psql(
		'postgres', qq[
		SELECT count(*) FROM (
		  SELECT (regexp_match(block_ref, 'blk (\\d+)'))[1]::int AS blk
		  FROM pg_get_wal_records_info('$wal_start', pg_current_wal_lsn())
		  WHERE resource_manager = 'Btree' AND record_type = 'VACUUM'
		) s WHERE blk = $P]);

	# Record whether the cursor was cancelled by any recovery conflict, and
	# specifically by the buffer-pin conflict (the safe interlock).  For the BUG
	# routes these are only diagnostics -- the pass/fail check is purely the
	# absence of phantoms -- but the CONTROL asserts on them.
	my $conflict_any =
	  $node_standby->log_contains(qr/conflict with recovery/, $logpos);
	my $conflict_pin = $node_standby->log_contains(
		qr/User was holding shared buffer pin for too long/, $logpos);
	note "$tbl: XLOG_BTREE_VACUUM records on pinned page P=$P: $vacuum_on_P; "
	  . "cursor hit a recovery conflict: "
	  . ($conflict_any ? "yes" : "no");

	my $fall = '';
	unless ($conflict_any)
	{
		($fall, my $ferr) = $sb->query('FETCH ALL FROM c;');
		$sb->query('ROLLBACK;') unless $ferr;
	}
	$sb->quit;

	# Ground truth: which a>=1 values legitimately exist on the primary now.  The
	# phantom value is gone (aborted or deleted-then-recycled), so every a=PHANTOM
	# the standby cursor returns is a phantom.
	my $truth = $node_primary->safe_psql('postgres',
		"SELECT a FROM $tbl WHERE a >= 1 ORDER BY a;");
	my %truth = map { $_ => 1 } ($truth =~ /^(-?\d+)$/mg);
	my @phantoms =
	  grep { $_ == $PHANTOM && !$truth{$_} } ($fall =~ /^(-?\d+)$/mg);

	# Summarise the wrongly-visible rows for an on-failure diagnostic: each
	# distinct value the scan returned that does not exist on the primary, with
	# how many times it came back.
	my %byval;
	$byval{$_}++ for @phantoms;
	my $phantom_detail = join(
		"\n",
		map {
			"    a=$_ -- returned $byval{$_} time(s), but no such row exists on the primary"
		} sort { $a <=> $b } keys %byval);

	return {
		vacuum_on_P => $vacuum_on_P,
		conflict_any => $conflict_any,
		conflict_pin => $conflict_pin,
		phantoms => scalar @phantoms,
		phantom_detail => $phantom_detail,
	};
}

# Run a BUG scenario in isolation and check the correctness property: the
# standby index-only scan must return NO phantom rows.  The whole scenario runs
# under eval, so a setup failure fails just this scenario and leaves the others
# to run and report their own results.  On failure, print the rows the scan
# wrongly returned.
sub bug_scenario
{
	my ($tbl, $route, $mode, $label) = @_;
	my $r = eval { run_scenario($tbl, $route, $mode) };
	unless (defined $r)
	{
		fail("$label: scenario did not run to completion");
		diag($@) if $@;
		return;
	}
	my $ok = is($r->{phantoms}, 0,
		"$label: standby index-only scan returned no phantom rows");
	diag(
		"$label: the standby IOS returned these rows -- visible only because "
		  . "of the all-visible bit, but absent on the primary (index entries "
		  . "for recycled heap TIDs):\n"
		  . $r->{phantom_detail})
	  unless $ok;
	return;
}

# ===========================================================================
# BUG via page split: the dead group moves to a right sibling (README's "items
# moved right"); the buffered entries leave P without an XLOG_BTREE_VACUUM on P.
# ===========================================================================
note "===== BUG: page split moves the dead group off the pinned page =====";
bug_scenario('t_split', 'split', 'abort', 'split/abort');

# ===========================================================================
# BUG via simple deletion: the dead entries are removed from P as XLOG_BTREE_DELETE
# (exclusive lock on replay), not XLOG_BTREE_VACUUM (cleanup lock).  Shown with
# aborted phantoms and, again, with a committed DELETE -- the aborted xact only
# makes recycling trivial, it is not the cause.
# ===========================================================================
note "===== BUG: simple deletion removes the dead entries in place =====";
bug_scenario('t_simpledel', 'simpledel', 'abort', 'simpledel/abort');
bug_scenario('t_simpledel_d', 'simpledel', 'delete', 'simpledel/delete');

# ===========================================================================
# CONTROL: the dead entries stay on P; VACUUM removes them there, emits
# XLOG_BTREE_VACUUM for P, and replay cleanup-locks P -> the cursor is cancelled.
# This always passes (bug present or fixed): it confirms the interlock works for
# the on-page case, so a BUG-route failure is a real defect, not a setup artifact.
# ===========================================================================
note
  "===== CONTROL: dead entries stay on the pinned page (interlock holds) =====";
my $ctl = eval { run_scenario('t_ctl', 'onpage', 'abort') };
if (defined $ctl)
{
	cmp_ok($ctl->{vacuum_on_P}, '>', '0',
		'CONTROL: VACUUM emitted XLOG_BTREE_VACUUM for the pinned page');
	ok($ctl->{conflict_pin},
		'CONTROL: cursor cancelled by a buffer-pin recovery conflict (interlock held)'
	);
	is($ctl->{phantoms}, 0, 'CONTROL: no phantom rows (the safe outcome)');
}
else
{
	fail("CONTROL: scenario did not run to completion");
	diag($@) if $@;
}

$node_standby->stop;
$node_primary->stop;
done_testing();
