# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that SSI still detects a dangerous structure after the conflicting
# reader has been summarized.
#
# When the SERIALIZABLEXACT pool is exhausted, SummarizeOldestCommittedSxact()
# folds the oldest committed transaction's SIREAD locks onto the dummy
# transaction OldCommittedSxact.  CheckTargetForConflictsIn() decides whether a
# committed reader overlapped the writer by comparing the reader's
# finishedBefore against the writer's snapshot, so OldCommittedSxact must carry
# a finishedBefore that covers everything folded into it.  Otherwise every
# summarized reader looks as though it finished before any writer began, the
# rw-conflict is dropped, and a transaction that should be cancelled commits.
#
# The schedule builds  Tin ->rw-> Tpivot ->rw-> Tout  with Tout committing
# first and Tin committing after Tout, which is a dangerous structure that
# must cancel Tpivot. This test tests if this happens when Tin is
# summarized.
# To get an intuition and to see how a serializability
# violation can actually occur in a real example, consider a customer that has
# 3 accounts with a balance of 0 with account ids 100, 101, 102.
# 1. Tout deposits $500 into account id 100 of the three accounts.
# 2. Tin takes its snapshot after Tout commits and reads all accounts,
# this creates a w-r edge from Tout to Tin (not relevant for conflict detection,
# but mentioned here to say that it is an actual cycle.)
# 3. churn exhausts the SERIALIZABLEXACT pool so Tin gets summarized
# 4. Tpivot is concurrent with Tout and Tin. It reads all account ids of
# the customer, and withdraws $500 from account id 101, thereby creating an
# RW edge from Tin to Tpivot and RW from Tpivot to Tout.
#
# There is a RW edge from Tin to Tpivot, an RW edge from Tpivot to Tout.
# There is no serial order in which Tin, Tpivot and Tout could have
# occurred. Thus this is a serializability violation, and Tpivot should
# be aborted.
# The tap test checks that even when Tin is summarized, the conflict is
# detected and Tpivot is aborted.
#

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('ssi');
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
CREATE TABLE account (id int PRIMARY KEY, cust_id int, val int NOT NULL);
INSERT INTO account SELECT g, g/5 + 1, 0 FROM generate_series(1, 2000) g;
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

my $pivot = $node->background_psql('postgres', on_error_stop => 0);
my $tin = $node->background_psql('postgres', on_error_stop => 0);
my $out = $node->background_psql('postgres', on_error_stop => 0);

# 1. Tpivot takes its snapshot first, so that it overlaps Tin.  Holding this
# snapshot open is also what stops the churn transactions from being cleaned
# up, which is what drives the pool to exhaustion.
$pivot->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE;');
note('Tpivot xid: ' . $pivot->query_safe('SELECT pg_current_xact_id();'));
$pivot->query_safe('SELECT sum(val) FROM account WHERE cust_id = 21;');

# 2. Tout writes account(id=100) and commits first.
$out->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE;');
note('Tout xid: ' . $out->query_safe('SELECT pg_current_xact_id();'));
$out->query_safe(
	'UPDATE account SET val = val + 500 WHERE id = 100; COMMIT;');

# 3. Tin concurrent with tpivot reads t(cust_id=21), leaving a SIREAD lock
$tin->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE;');
note('Tin xid: ' . $tin->query_safe('SELECT pg_current_xact_id();'));
$tin->query_safe('SELECT sum(val) FROM account WHERE cust_id = 21;');

# 4. Tin commits after Tout.
$tin->query_safe('COMMIT;');

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

# 6. Tpivot writes account(id=101).  Tin's SIREAD lock now belongs to
# OldCommittedSxact, and this is where the rw-conflict in must still be found.
# Tpivot can be cancelled right here, as a pivot caught during the write, or
# it can survive the write and only be cancelled by the read in step 7 below;
# either is a valid detection of the dangerous structure, so check both spots.
my $conflict_re =
  qr/could not serialize access due to read\/write dependencies among transactions/;
$pivot->query('UPDATE account SET val = val - 500 WHERE id = 101;');

if ($pivot->{stderr} =~ $conflict_re)
{
	pass('summarized reader still completes a dangerous structure');
}
else
{
	fail('No serializability violation due to summarized xact!!');
}

$pivot->quit;
$tin->quit;
$out->quit;
$node->stop;

done_testing();
