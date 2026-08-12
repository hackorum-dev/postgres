# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Demonstrate that a subtransaction which aborts after it has already
# subcommitted would corrupt a primary key without the childXids restore
# in AbortSubTransaction().
#
# AtSubCommit_childXids() copies the subtransaction's XID into the parent's
# committed-children array before CommitSubTransaction() is finished.  The
# test_subxact_commit.force_error GUC makes the COMMIT_SUB callback raise
# after that transfer.  Control longjmps into AbortSubTransaction(), which
# records the XID aborted but, without the fix, leaves it in the parent's
# array.  The parent's commit then marks that aborted XID COMMITTED, so a
# row the subtransaction rolled back becomes live.
#
# To make that reach the index we use a HOT update.  The rolled-back row sits
# in the HOT chain behind a live one, so it has no index entry of its own and
# nobody looks at its xmin while the XID is still aborted -- had anyone done
# so, the HEAP_XMIN_INVALID hint bit would have masked the resurrection.  A
# second session then updates the row, which retires the version the
# rolled-back one was chained behind.  Once the culprit commits, both the
# resurrected row and the second session's row would be live under the same
# key without the fix.
#
# The checks below assert correct behaviour, so on unpatched non-cassert
# builds they fail and print the corruption.  A build with --enable-cassert
# trips the assertion in clog.c on the culprit COMMIT instead.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('subxact_resurrect_pk');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

# Extension is installed with this test module.
$node->safe_psql('postgres', 'CREATE EXTENSION test_subxact_commit;');
$node->safe_psql('postgres', 'CREATE EXTENSION amcheck;');
$node->safe_psql('postgres', 'CREATE EXTENSION pageinspect;');
$node->safe_psql('postgres', q[
CREATE TABLE t (id int PRIMARY KEY, note text);
INSERT INTO t VALUES (1, 'original');
]);

# The offending session.  force_error raises from COMMIT_SUB after the XID
# has been handed to the parent, so the subtransaction aborts while already
# in TRANS_COMMIT state.  The PL/pgSQL EXCEPTION block swallows that error
# and the surrounding transaction lives on.
my $culprit = $node->background_psql('postgres', on_error_stop => 0);

# Load the module in this backend.  SET of a custom GUC alone only creates a
# placeholder if the .so is not loaded, so the COMMIT_SUB callback would never
# run.
$culprit->query_safe(q[SELECT test_subxact_commit_init()]);

$culprit->query_safe(q[BEGIN]);

# note is not indexed and the page has room, so this is a HOT update.
$culprit->query(q[
DO $$
BEGIN
  BEGIN
    SET LOCAL test_subxact_commit.force_error = on;
    UPDATE t SET note = 'rolled-back-subxact' WHERE id = 1;
  EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'subxact aborted: %', SQLERRM;
  END;
END $$;
]);

# The WARNING and NOTICE above arrive on stderr; take them and clear it, so
# that the later query_safe() calls do not mistake them for a failure.
my $subxact_err = $culprit->{stderr};
$culprit->{stderr} = '';

like(
	$subxact_err,
	qr/AbortSubTransaction while in COMMIT state/,
	'subtransaction aborted after it had already subcommitted');

# A second session updates the same row.  It sees the original version as
# live, since the subtransaction that tried to supersede it is aborted, and
# never examines the rolled-back version behind it.
$node->safe_psql('postgres', "UPDATE t SET note = 'other-session' WHERE id = 1;");

# Releasing the culprit is what does the damage without the fix: its commit
# record carries the aborted subtransaction in the child list, so that XID
# is marked COMMITTED.
$culprit->query_safe(q[COMMIT]);
$culprit->quit;

diag("heap page after commit:\n"
	  . $node->safe_psql('postgres', q[
SELECT lp, t_xmin, t_xmax, t_ctid,
       (t_infomask & 256) <> 0 AS xmin_committed,
       (t_infomask & 512) <> 0 AS xmin_invalid,
       (t_infomask & 1024) <> 0 AS xmax_committed,
       (t_infomask & 2048) <> 0 AS xmax_invalid
FROM heap_page_items(get_raw_page('t', 0)) ORDER BY lp;]));

# A sequential scan and an index scan must agree on how many rows carry the key.
my $seqscan = $node->safe_psql('postgres', q[
SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_indexonlyscan = off;
SELECT count(*) FROM t WHERE id = 1;]);
my $idxscan = $node->safe_psql('postgres', q[
SET enable_seqscan = off;
SELECT count(*) FROM t WHERE id = 1;]);

diag("rows with id = 1: seqscan $seqscan, index scan $idxscan");

is($seqscan, '1', 'primary key must hold a single row for the key');
is($seqscan, $idxscan, 'sequential and index scan must agree');

# amcheck compares the index against the heap under a snapshot, so a live
# heap tuple with no index entry, or two live entries under a unique key, is
# reported as corruption.
my ($rc, $stdout, $stderr) = $node->psql('postgres',
	"SELECT bt_index_check(index => 't_pkey'::regclass, heapallindexed => true, checkunique => true);"
);

diag("amcheck says: $stderr") if $stderr ne '';
is($rc, 0, 'amcheck must find the primary key intact');

# And rebuilding the index must not stumble over a duplicate.
my ($rc2, $stdout2, $stderr2) =
  $node->psql('postgres', 'REINDEX TABLE t;');

diag("reindex says: $stderr2") if $stderr2 ne '';
is($rc2, 0, 'the primary key must be rebuildable');

# The damage reached WAL in the parent's commit record, so recovery would
# apply the same wrong status without the fix.
$node->stop('immediate');
$node->start;

my $seqscan_after = $node->safe_psql('postgres', q[
SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_indexonlyscan = off;
SELECT count(*) FROM t WHERE id = 1;]);

is($seqscan_after, '1', 'the key must still hold a single row after recovery');

done_testing();
