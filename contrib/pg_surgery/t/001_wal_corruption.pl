# Copyright (c) 2024-2026, PostgreSQL Global Development Group
#
# Test for the VM buffer locking fix in heap_force_kill.
#
# heap_force_kill writes two FPIs per page when the page is all-visible:
# one for the heap page and one for the visibility map page.  The VM
# buffer lock is re-acquired after visibilitymap_clear() (which releases
# it) and held through the log_newpage_buffer(vmbuf) call.  This
# prevents concurrent modification during XLogInsert, which reads the
# page twice: once for CRC in XLogRecordAssemble and once for the data
# copy in CopyXLogRecordToWAL.
#
# Without the fix, concurrent modification of the VM page between those
# two reads produces a CRC mismatch -- the exact WAL corruption observed
# in production under 50 concurrent heap_force_kill sessions.
#
# This test uses three injection points:
#
#   1. "heap-force-kill-vm-pin" -- outside critical section, forces DSM
#      shared memory initialization for the wait machinery.
#
#   2. "heap-force-kill-before-vm-wal" -- inside critical section, between
#      the heap page FPI and VM page FPI writes.  Used as a synchronization
#      barrier so the test knows the heap FPI is complete.
#
#   3. "wal-insert-after-crc" -- inside XLogInsert, between
#      XLogRecordAssemble (CRC done) and XLogInsertRecord (data copy).
#      This fires twice: once for the heap FPI, once for the VM FPI.
#      With the fix, the VM buffer lock is held during the VM FPI's
#      XLogInsert, so concurrent modification cannot corrupt the CRC.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (!defined($ENV{enable_injection_points})
	|| $ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
if (!$node->check_extension('pg_surgery'))
{
	plan skip_all => 'Extension pg_surgery not installed';
}
if (!$node->check_extension('pg_walinspect'))
{
	plan skip_all => 'Extension pg_walinspect not installed';
}

$node->safe_psql('postgres', q{CREATE EXTENSION injection_points});
$node->safe_psql('postgres', q{CREATE EXTENSION pg_surgery});
$node->safe_psql('postgres', q{CREATE EXTENSION pg_walinspect});

# Create a table with enough rows to span at least two heap pages.
# With a single int column (~28 bytes/tuple including header), each 8kB
# page holds ~226 tuples.  500 rows gives us pages 0, 1, and 2 -- all
# mapping to VM page 0 (which covers heap blocks 0 through 32767).
$node->safe_psql('postgres', q{
	CREATE TABLE test_vm (a int);
	INSERT INTO test_vm SELECT generate_series(1, 500);
});

# VACUUM to set all pages all-visible.  This is required for
# heap_force_kill to enter the VM modification path (PageIsAllVisible
# must be true).
$node->safe_psql('postgres', q{VACUUM test_vm});

# Verify that we have tuples on at least two distinct pages.
my $page0_count = $node->safe_psql('postgres',
	q{SELECT count(*) FROM test_vm WHERE ctid >= '(0,1)' AND ctid < '(1,0)'});
my $page1_count = $node->safe_psql('postgres',
	q{SELECT count(*) FROM test_vm WHERE ctid >= '(1,1)' AND ctid < '(2,0)'});
cmp_ok($page0_count, '>', 0, 'page 0 has tuples');
cmp_ok($page1_count, '>', 0, 'page 1 has tuples');

# Record WAL position before the test so we can validate WAL afterward.
my $before_lsn = $node->safe_psql('postgres',
	q{SELECT pg_current_wal_lsn()});

# Session 1: attach all three injection points (local to this PID) and
# run heap_force_kill on a tuple on page 0.
my $s1 = $node->background_psql('postgres');
$s1->query_safe(q{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('heap-force-kill-vm-pin', 'wait');
	SELECT injection_points_attach('heap-force-kill-before-vm-wal', 'wait');
	SELECT injection_points_attach('wal-insert-after-crc', 'wait');
});

$s1->query_until(
	qr/starting_heap_force_kill/,
	q(\echo starting_heap_force_kill
SELECT heap_force_kill('test_vm'::regclass, ARRAY['(0,1)']::tid[]);
));

# ---- Pause 1: "heap-force-kill-vm-pin" (outside critical section) ----
# Forces DSM initialization for the injection_wait machinery.
$node->wait_for_event('client backend', 'heap-force-kill-vm-pin');
$node->safe_psql('postgres',
	q{SELECT injection_points_wakeup('heap-force-kill-vm-pin')});
ok(1, 'pause 1: DSM initialized via heap-force-kill-vm-pin');

# ---- Pause 2: "wal-insert-after-crc" (heap FPI's XLogInsert) ----
# Session 1 entered the critical section, marked tuples dead, called
# visibilitymap_clear, re-acquired the VM lock, and is now inside
# XLogInsert for the HEAP page FPI.  CRC has been computed over the
# heap page; the page is still locked via LockBufferForCleanup so no
# concurrent modification is possible.  Just wake it up.
$node->wait_for_event('client backend', 'wal-insert-after-crc');
$node->safe_psql('postgres',
	q{SELECT injection_points_wakeup('wal-insert-after-crc')});
ok(1, 'pause 2: heap FPI CRC computed, waking to continue');

# ---- Pause 3: "heap-force-kill-before-vm-wal" (between FPIs) ----
# The heap FPI has been written to WAL.  Session 1 is now between the
# heap FPI and the VM FPI.  With the fix, the VM buffer is LOCKED
# (re-acquired after visibilitymap_clear).
$node->wait_for_event('client backend', 'heap-force-kill-before-vm-wal');
ok(1, 'pause 3: session 1 paused between heap FPI and VM FPI');

# Wake session 1 from the barrier.  It will proceed to
# log_newpage_buffer(vmbuf) -> XLogInsert -> XLogRecordAssemble (CRC)
# -> hit "wal-insert-after-crc" again.
$node->safe_psql('postgres',
	q{SELECT injection_points_wakeup('heap-force-kill-before-vm-wal')});

# ---- Pause 4: "wal-insert-after-crc" (VM FPI's XLogInsert) ----
# Session 1 has computed the CRC over the VM page in
# XLogRecordAssemble, but has NOT yet copied the data to WAL buffers
# in CopyXLogRecordToWAL.  With the fix, the VM buffer is exclusively
# locked, so session 2 will block.  Without the fix, session 2 can
# modify the VM page and cause a CRC mismatch.
$node->wait_for_event('client backend', 'wal-insert-after-crc');
ok(1, 'pause 4: VM FPI CRC computed');

# Session 2: run heap_force_kill on page 1 (same VM page) IN THE
# BACKGROUND while session 1 is paused inside XLogInsert.  This is
# the critical concurrent modification:
#
#   - With the fix: session 2 blocks on LockBuffer(vmbuf) inside
#     visibilitymap_clear because session 1 holds the VM lock.
#     No corruption.
#
#   - Without the fix: session 2 completes, modifying the VM page
#     between session 1's CRC computation and data copy.
#     CRC mismatch.
my $s2 = $node->background_psql('postgres');
my $s2_pid = $s2->query_safe(q{SELECT pg_backend_pid()});
chomp $s2_pid;

$s2->query_until(
	qr/starting_s2/,
	q(\echo starting_s2
SELECT heap_force_kill('test_vm'::regclass, ARRAY['(1,1)']::tid[]);
\echo s2_done
));

# Wait for session 2 to either block on the VM buffer lock (fix
# applied) or complete and become idle (no fix).  This replaces a
# fixed sleep with a deterministic poll, making the test CI-stable.
use Time::HiRes qw(usleep);
my $observed = '';
my $deadline = time() + 10;
while (time() < $deadline)
{
	$observed = $node->safe_psql('postgres', qq{
		SELECT format('%s/%s/%s',
			coalesce(wait_event_type, 'NULL'),
			coalesce(wait_event, 'NULL'),
			coalesce(state, 'NULL'))
		FROM pg_stat_activity WHERE pid = $s2_pid
	});
	last if $observed =~ m{^Buffer/BufferExclusive/}
		|| $observed =~ m{/idle$};
	usleep(50_000);
}

# Now wake session 1.  It will proceed to CopyXLogRecordToWAL.
# If session 2 already modified the VM page (unfixed), the CRC won't
# match.  If session 2 is blocked (fixed), the page is stable.
$node->safe_psql('postgres', q{
	SELECT injection_points_detach('wal-insert-after-crc');
	SELECT injection_points_wakeup('wal-insert-after-crc');
});

# Wait for both sessions to finish.
$s1->query_until(qr/heap_force_kill_done/,
	q(\echo heap_force_kill_done
));
$s1->quit;

$s2->query_until(qr/s2_done/, '');
$s2->quit;

# Both tuples should be dead (not visible via sequential scan).
my $dead_count = $node->safe_psql('postgres',
	q{SELECT count(*) FROM test_vm WHERE ctid IN ('(0,1)', '(1,1)')});
is($dead_count, '0', 'both target tuples are dead');

# Validate WAL with pg_walinspect.  With the fix (VM buffer locked
# through log_newpage_buffer), the VM FPI should have a valid CRC.
# Without the fix, this would report "incorrect resource manager data
# checksum" because session 2 modified the VM page between CRC
# computation and data copy.
#
# Force a CHECKPOINT first so all WAL is flushed and readable by
# pg_walinspect (it reads up to GetFlushRecPtr).
$node->safe_psql('postgres', q{CHECKPOINT});

my ($ret, $stdout, $stderr) = $node->psql('postgres', qq{
	SELECT count(*)
	FROM pg_get_wal_records_info('$before_lsn', pg_current_wal_lsn());
});
is($ret, 0, 'pg_walinspect reads all WAL without error');
unlike($stderr, qr/incorrect resource manager data checksum/i,
	'no CRC mismatch in WAL (VM buffer locking fix works)');
cmp_ok($stdout, '>', 0,
	'WAL contains records from heap_force_kill operations');

$node->stop;

done_testing();
