# Copyright (c) 2024-2026, PostgreSQL Global Development Group
#
# Test: reproduce the exported-snapshot xmin handoff race.
#
# Strategy (two-injection-point approach, for the exclusive-lock fix):
#   1. VACUUM attaches "get-oldest-nonremovable-txid" with injection_wait
#      (PID-filtered via injection_points_set_local).
#   2. Start VACUUM.  The first ComputeXidHorizons call (from on-access
#      catalog pruning via GlobalVisUpdate) passes through unblocked.
#   3. VACUUM reaches GetOldestNonRemovableTransactionId -> blocked at
#      "get-oldest-nonremovable-txid".
#   4. Coordinator detects the block -> GLOBALLY attaches
#      "compute-xid-horizons-at-1" with injection_wait (no PID filter).
#   5. Coordinator wakes VACUUM from "get-oldest-nonremovable-txid".
#   6. VACUUM enters ComputeXidHorizons (holding ProcArrayLock shared)
#      -> blocked at "compute-xid-horizons-at-1" (after scanning importer
#        at index 0 with xmin=Invalid).
#   7. Coordinator detects the second block, detaches the injection point so
#      later snapshots do not hit it, and starts SET TRANSACTION SNAPSHOT
#      asynchronously.  The fix makes ProcArrayInstallImportedXmin wait for
#      ProcArrayLock exclusive instead of installing xmin while VACUUM is in
#      the middle of a proc-array scan.
#   8. Coordinator verifies that the importer is waiting, then wakes
#      "compute-xid-horizons-at-1".
#   9. VACUUM continues while the source transaction is still open, so its
#      horizon remains low enough to preserve the deleted tuple.  Afterwards
#      the importer installs the snapshot.
#  10. Importer queries -> 1 row.
#
# Without the fix, SET TRANSACTION SNAPSHOT completes under a shared
# ProcArrayLock while VACUUM is paused, so the lock-wait assertion fails.
#
# Depends only on: injection_points (built-in test module)

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('export_race_node');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'injection_points'");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

$node->safe_psql('postgres', qq{
	CREATE TABLE race_test (id int, data text);
	INSERT INTO race_test VALUES (1, 'should_be_visible');
});

# ComputeXidHorizons iterates over pgprocnos[], which contains only
# client backends that called ProcArrayAdd().  The test relies on the importer
# preceding the source transaction, with at least one entry between them.
my $imp   = $node->background_psql('postgres');  # index 0
my $fill  = $node->background_psql('postgres');  # index 1 (gap)
my $src   = $node->background_psql('postgres');  # index 2 (source)
my $vac   = $node->background_psql('postgres');  # index 3 (VACUUM)
my $del   = $node->background_psql('postgres');  # index 4 (deleter)
my $coord = $node->background_psql('postgres');

my $imp_pid = $imp->query("SELECT pg_backend_pid()");
$imp_pid =~ s/\s+//g;

$src->query("BEGIN ISOLATION LEVEL REPEATABLE READ");
$src->query("SELECT * FROM race_test");
my $token = $src->query("SELECT pg_export_snapshot()");
$token =~ s/\s+//g;
diag("exported snapshot token: $token");

$del->query("DELETE FROM race_test WHERE id = 1");
$del->query("COMMIT");
diag("deleter committed");

# Advance the XID counter so that the horizon (latestCompletedXid + 1 when
# no backend has a valid xmin) is strictly greater than the deleter's XID.
for (my $i = 0; $i < 100; $i++)
{
	$node->safe_psql('postgres', "SELECT txid_current()");
}
diag("100 filler XIDs consumed");

# No query after BEGIN: importer xmin stays Invalid until SET TRANSACTION
# SNAPSHOT, which is essential for this race.
$imp->query("BEGIN ISOLATION LEVEL REPEATABLE READ");

my $vac_pid = $vac->query("SELECT pg_backend_pid()");
$vac_pid =~ s/\s+//g;
diag("VACUUM PID: $vac_pid");

$vac->query("SELECT injection_points_set_local()");
$vac->query(
	"SELECT injection_points_attach('get-oldest-nonremovable-txid', 'wait')");
diag("VACUUM attached get-oldest-nonremovable-txid");

$vac->query_until(qr/vac_started/,
	"\\echo vac_started\nVACUUM race_test;\n");
diag("VACUUM started");

{
	my $blocked = 0;
	for (my $i = 0; $i < 1800; $i++)
	{
		my $result = $coord->query(
			"SELECT count(*) = 1 FROM pg_stat_activity"
			  . " WHERE pid = $vac_pid"
			  . " AND wait_event_type = 'InjectionPoint'");
		if ($result =~ /t/)
		{
			$blocked = 1;
			last;
		}
		usleep(100_000);
	}
	die "VACUUM did not reach first injection point within 180s"
		unless $blocked;
}
diag("VACUUM blocked at get-oldest-nonremovable-txid");

$coord->query(
	"SELECT injection_points_attach('compute-xid-horizons-at-1', 'wait')");
diag("coordinator attached compute-xid-horizons-at-1");

$coord->query(
	"SELECT injection_points_wakeup('get-oldest-nonremovable-txid')");
diag("woke VACUUM from get-oldest-nonremovable-txid");

{
	my $blocked = 0;
	for (my $i = 0; $i < 1800; $i++)
	{
		my $result = $coord->query(
			"SELECT count(*) = 1 FROM pg_stat_activity"
			  . " WHERE pid = $vac_pid"
			  . " AND wait_event_type = 'InjectionPoint'");
		if ($result =~ /t/)
		{
			$blocked = 1;
			last;
		}
		usleep(100_000);
	}
	die "VACUUM did not reach second injection point within 180s"
		unless $blocked;
}
diag("VACUUM blocked at compute-xid-horizons-at-1");

$coord->query(
	"SELECT injection_points_detach('compute-xid-horizons-at-1')");
diag("detached compute-xid-horizons-at-1 while VACUUM remains blocked");

# An exported snapshot that is never imported must retain the normal no-XID
# commit path: it should not wait for ProcArrayLock.
$fill->query("BEGIN ISOLATION LEVEL REPEATABLE READ");
my $unused_token = $fill->query("SELECT pg_export_snapshot()");
$unused_token =~ s/\s+//g;

my $fill_pid = $fill->query("SELECT pg_backend_pid()");
$fill_pid =~ s/\s+//g;
$fill->query_until(qr/unreferenced_commit_started/,
	"\\echo unreferenced_commit_started\nCOMMIT;\n"
	  . "\\echo unreferenced_commit_done\n");

my $unreferenced_finished = 0;
for (my $i = 0; $i < 100; $i++)
{
	my $result = $coord->query(
		"SELECT CASE"
		  . " WHEN state = 'idle' THEN 'done'"
		  . " WHEN wait_event_type = 'LWLock'"
		  . " AND wait_event = 'ProcArray' THEN 'blocked'"
		  . " ELSE 'running' END"
		  . " FROM pg_stat_activity WHERE pid = $fill_pid");
	if ($result =~ /done/)
	{
		$unreferenced_finished = 1;
		last;
	}
	last if $result =~ /blocked/;
	usleep(100_000);
}
diag("unreferenced export started COMMIT while VACUUM holds ProcArrayLock");

$imp->query_until(qr/import_started/,
	"\\echo import_started\nSET TRANSACTION SNAPSHOT '$token';\n"
	  . "\\echo import_done\n");
diag("importer started SET TRANSACTION SNAPSHOT");

my $importer_waiting = 0;
for (my $i = 0; $i < 100; $i++)
{
	my $result = $coord->query(
		"SELECT state || '|' || COALESCE(wait_event_type, '') || '|' ||"
		  . " COALESCE(wait_event, '')"
		  . " FROM pg_stat_activity"
		  . " WHERE pid = $imp_pid");
	if ($result =~ /active\|LWLock\|ProcArray/)
	{
		$importer_waiting = 1;
		last;
	}
	last if $result eq '';
	usleep(100_000);
}

$coord->query(
	"SELECT injection_points_detach('get-oldest-nonremovable-txid')");
$coord->query(
	"SELECT injection_points_wakeup('compute-xid-horizons-at-1')");
diag("detached remaining injection point and woke VACUUM");

$imp->query_until(qr/import_done/, "");
$src->query("COMMIT");
$fill->query_until(qr/unreferenced_commit_done/, "");

ok($unreferenced_finished,
	"unreferenced export clears xmin without waiting for ProcArrayLock");
ok($importer_waiting,
	"importing transaction waits for ProcArrayLock while VACUUM computes horizons");

{
	my $done = 0;
	for (my $i = 0; $i < 1800; $i++)
	{
		my $result = $coord->query(
			"SELECT count(*) = 1 FROM pg_stat_activity"
			  . " WHERE pid = $vac_pid"
			  . " AND state = 'idle'");
		if ($result =~ /t/)
		{
			$done = 1;
			last;
		}
		usleep(100_000);
	}
	die "VACUUM did not finish within 180s" unless $done;
}
diag("VACUUM finished");

my $count = $imp->query("SELECT count(*) FROM race_test");
$count =~ s/\s+//g;
diag("importer sees $count row(s)");

is($count, 1,
	"imported snapshot still sees the row after concurrent VACUUM")
  or diag("BUG DETECTED: export-snapshot xmin race caused "
		. "premature tuple removal (expected 1 row, got $count)");

$imp->query("COMMIT");

$node->stop;
done_testing();
