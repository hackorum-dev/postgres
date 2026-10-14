# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that SSI still detects a dangerous structure after the conflicting
# reader has been summarized, when the writer got its snapshot by importing
# it with SET TRANSACTION SNAPSHOT.
#
# When the SERIALIZABLEXACT pool is exhausted, SummarizeOldestCommittedSxact()
# folds the oldest committed transaction's SIREAD locks onto the dummy
# transaction OldCommittedSxact.  Each such lock retains the latest commit
# sequence number among the transactions folded into it.
# CheckTargetForConflictsIn() decides whether a summarized reader overlapped
# the writer by comparing that number with the writer's
# SeqNo.lastCommitBeforeSnapshot: if the reader committed after it, the two
# overlapped.
#
# A transaction that imports a snapshot has a snapshot which was taken earlier
# by the exporting transaction.  Its lastCommitBeforeSnapshot has to be the
# exporter's, not the value of PredXact->LastSxactCommitSeqNo at the time of
# the import.  Otherwise a reader which committed between the export and the
# import looks as though it committed before the snapshot, although the
# snapshot cannot see it, and the rw-conflict from the reader is dropped.
#
# The schedule builds  Tin ->rw-> Tpivot ->rw-> Tout  with Tout committing
# first and Tin committing after Tout, which is a dangerous structure that
# must cancel Tpivot:
#
# 1. Texport takes a snapshot and exports it.  It stays open, which also stops
#    the churn below from being cleaned up.
# 2. Tout, which begins after the snapshot, writes account 100 and commits.
# 3. Tin, which begins after Tout committed, reads account 100, which gives
#    the wr-edge Tout -> Tin, and account 101.  It writes account 102, then
#    commits after Tout.
# 4. Tpivot starts, imports Texport's snapshot, and reads account 100.  The
#    snapshot predates Tout's commit, so it sees the old row version, giving
#    the rw-edge Tpivot -> Tout.  It starts after Tin committed, so
#    LastSxactCommitSeqNo already includes Tin's commit.
# 5. churn exhausts the SERIALIZABLEXACT pool, so Tin is summarized.
# 6. Tpivot writes account 101, giving the rw-edge Tin -> Tpivot.  Tin's
#    commit is not in Tpivot's snapshot, so the edge must be found even
#    though Tin's lock is now held by OldCommittedSxact.
#
# The dangerous structure alone is only a warning sign: SSI also cancels
# schedules which turn out to be serializable.  This schedule is a real
# anomaly, because the wr-edge Tout -> Tin closes the cycle
#
#   Tout ->wr-> Tin ->rw-> Tpivot ->rw-> Tout
#
# Tpivot saw account 100 without Tout's deposit, so Tpivot is before Tout;
# Tin saw Tout's deposit, so Tout is before Tin; Tin read account 101 before
# Tpivot changed it, so Tin is before Tpivot.  No serial order satisfies all
# three, so Tpivot has to be aborted.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('ssi_import');
$node->init;

# Keep MaxBackends small so that the SERIALIZABLEXACT pool, which holds
# (MaxBackends + max_prepared_xacts) * 10 entries, is cheap to exhaust.
$node->append_conf(
	'postgresql.conf', qq[
autovacuum = off
max_connections = 12
autovacuum_worker_slots = 1
max_worker_processes = 0
max_prepared_transactions = 0
]);
$node->start;

$node->safe_psql(
	'postgres', q[
CREATE TABLE account (id int PRIMARY KEY, val int NOT NULL);
INSERT INTO account SELECT g, 0 FROM generate_series(1, 2000) g;
VACUUM ANALYZE account;

-- Run many short serializable transactions.  While another session holds a
-- snapshot, none of them can be cleaned up, so the pool fills and
-- summarization begins.  They all read the same row, so the predicate lock
-- target table does not fill up as well.
CREATE PROCEDURE churn(n int) LANGUAGE plpgsql AS $$
BEGIN
  FOR i IN 1 .. n LOOP
    PERFORM val FROM account WHERE id = 1500;
    COMMIT;
  END LOOP;
END $$;
]);

my $export = $node->background_psql('postgres', on_error_stop => 0);
my $pivot = $node->background_psql('postgres', on_error_stop => 0);
my $tin = $node->background_psql('postgres', on_error_stop => 0);
my $out = $node->background_psql('postgres', on_error_stop => 0);

# 1. Texport takes its snapshot and exports it.  Holding this transaction open
# keeps the exported snapshot valid, and stops the churn transactions from
# being cleaned up, which is what drives the pool to exhaustion.
$export->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE;');
my $snapshot = $export->query_safe('SELECT pg_export_snapshot();');
chomp $snapshot;
note("exported snapshot $snapshot");

# 2. Tout writes account 100 and commits first.
$out->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE;');
$out->query_safe('UPDATE account SET val = val + 500 WHERE id = 100;');
$out->query_safe('COMMIT;');

# 3. Tin begins after Tout committed, and reads account 100, so it sees Tout's
# deposit: this is the wr-edge Tout -> Tin which makes the schedule a real
# anomaly rather than one SSI merely suspects.  It also reads account 101,
# leaving a SIREAD lock.  It writes too, because a transaction that never
# writes is treated as read-only, which exempts it from the dangerous
# structure.  Tin commits after Tout.
$tin->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE;');
is($tin->query_safe('SELECT val FROM account WHERE id = 100;'),
	'500', 'Tin sees the deposit made by Tout');
$tin->query_safe('SELECT val FROM account WHERE id = 101;');
$tin->query_safe('UPDATE account SET val = val + 1 WHERE id = 102;');
$tin->query_safe('COMMIT;');

# 4. Tpivot imports Texport's snapshot, which predates the commits of Tout and
# Tin, and reads account 100.  Tout has not been summarized yet, so the read of
# the old row version records the rw-edge Tpivot -> Tout.  (Were Tout already
# summarized, that edge would be forgotten and nothing would be left to detect.)
$pivot->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE;');
$pivot->query_safe("SET TRANSACTION SNAPSHOT '$snapshot';");
my $seen = $pivot->query_safe('SELECT val FROM account WHERE id = 100;');
is($seen, '0',
	'Tpivot reads the old row version through the imported snapshot');

# 5. Force Tin to be summarized.  Tin committed before any churn transaction,
# so it is at the head of the finished list and is summarized first.
#
# The pool holds (MaxBackends + max_prepared_xacts) * 10 entries, and
# MaxBackends is the sum of the settings below plus NUM_SPECIAL_WORKER_PROCS.
# Compute it rather than hard-coding, so that the churn stays short whatever
# the defaults are, and add a margin for the transactions run above.
my $pool = $node->safe_psql(
	'postgres', q[
SELECT (current_setting('max_connections')::int
	  + current_setting('autovacuum_worker_slots')::int
	  + current_setting('max_worker_processes')::int
	  + current_setting('max_wal_senders')::int
	  + 2
	  + current_setting('max_prepared_transactions')::int) * 10
]);
note("SERIALIZABLEXACT pool holds $pool entries");
$out->query_safe(
		"SET default_transaction_isolation = 'serializable'; CALL churn("
	  . ($pool + 100)
	  . ");");

# The test is only meaningful if summarization actually happened; without it
# this schedule is just the ordinary dangerous-structure case, which is
# detected either way.  A summarized lock belongs to OldCommittedSxact, which
# has no backend behind it, so it shows up in pg_locks with a null pid.
my $summarized = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_locks WHERE mode = 'SIReadLock' AND pid IS NULL"
);
cmp_ok($summarized, '>', 0,
	"summarization occurred ($summarized summarized SIREAD locks)");

# 6. Tpivot writes account 101.  Tin's SIREAD lock now belongs to
# OldCommittedSxact, and this is where the rw-conflict in must still be found.
# Tpivot can be cancelled right here, as a pivot caught during the write, or
# it can survive the write and only be cancelled at commit; either is a valid
# detection of the dangerous structure, so check both spots.
my $conflict_re =
  qr/could not serialize access due to read\/write dependencies among transactions/;
$pivot->query('UPDATE account SET val = val - 500 WHERE id = 101;');
my $failed = $pivot->{stderr} =~ $conflict_re;
if (!$failed)
{
	$pivot->query('COMMIT;');
	$failed = $pivot->{stderr} =~ $conflict_re;
}
ok($failed,
	'summarized reader still completes a dangerous structure for an imported snapshot'
);

$export->quit;
$pivot->quit;
$tin->quit;
$out->quit;
$node->stop;

done_testing();
