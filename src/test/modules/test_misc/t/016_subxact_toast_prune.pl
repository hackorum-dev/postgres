# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A subtransaction that aborts after it has already subcommitted destroys data
# that the surrounding transaction goes on to commit.
#
# AtSubCommit_childXids() hands the subtransaction's XID to the parent's
# committed-children array before CommitSubTransaction() has finished.  If a
# later step throws, control longjmps into AbortSubTransaction(), which marks
# the XID aborted in pg_xact and removes it from the proc array, but leaves it
# in the parent's array.  From then until the parent commits, that XID reads as
# neither in progress nor committed, so HeapTupleSatisfiesVacuumHorizon()
# returns HEAPTUPLE_DEAD for every tuple it wrote, and SetHintBits() stamps
# HEAP_XMIN_INVALID on them.  Both are one-way: the hint bit is never cleared,
# and pruning releases the tuple outright.  When the parent commits it rewrites
# the XID's pg_xact status to COMMITTED, and whatever survived becomes live
# again -- referring to whatever did not.
#
# Part 1 reproduces the field report at
# https://postgr.es/m/CAFj8pRDcenNE3qUf=6YsrhMLki7BbbM8fLMi9DUzLn_jbLtpGw@mail.gmail.com
# down to the shape of the damage:
#
#   - the owning heap row survives, visible, carrying HEAP_XMIN_COMMITTED and
#     the same xmin as the chunks that were destroyed;
#   - its TOAST chunks are pruned, over tuples whose xmax is 0, so the
#     xl_heap_prune record reads "latestRemovedXid 0 nredirected 0 ndead 4"
#     (HeapTupleHeaderAdvanceLatestRemovedXid() advances latestRemovedXid only
#     for a tuple whose xmin committed, and here it had not);
#   - reading the row then fails with "missing chunk number 0 for toast value",
#     and verify_heapam reports "toast value ... not found in toast table".
#
# The heap row survives because vacuum_rel() vacuums a relation's TOAST table
# only after it is done with the main relation, in a separate transaction.  The
# test opens the window between those two passes, so the main pass sees the
# tuple as INSERT_IN_PROGRESS and leaves it alone, and only the TOAST pass sees
# it as dead.  DISABLE_PAGE_SKIPPING is used throughout so that nothing here
# depends on visibility-map behaviour.
#
# Part 2 shows the same primitive with no TOAST and no pruning at all: a plain
# SELECT during the window is enough to lose rows of a committed transaction
# permanently, across a checkpoint and a restart.
#
# The checks assert correct behaviour, so they fail on unpatched code.  Build
# without --enable-cassert: an assert-enabled build trips the assertion in
# clog.c at the parent's commit instead of reaching any of this.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('subxact_toast_prune');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');
$node->safe_psql('postgres', 'CREATE EXTENSION amcheck;');
$node->safe_psql('postgres', 'CREATE EXTENSION pageinspect;');

# EXTERNAL storage keeps the value uncompressed so that it really occupies
# TOAST chunks, and 7900 bytes fills one TOAST page with four of them -- the
# same shape as the page in the field report.
$node->safe_psql(
	'postgres', q[
CREATE TABLE audit_log (id int PRIMARY KEY, payload text);
ALTER TABLE audit_log ALTER COLUMN payload SET STORAGE EXTERNAL;
CREATE TABLE plain (id int PRIMARY KEY, note text);
]);

my $toastrel = $node->safe_psql('postgres',
	q[SELECT reltoastrelid::regclass::text FROM pg_class WHERE oid = 'audit_log'::regclass]
);

sub heap_flags
{
	my ($rel) = @_;
	return $node->safe_psql('postgres', qq[
SELECT coalesce(string_agg(format('lp %s: flags %s xmin %s %s', lp, lp_flags,
                                  coalesce(t_xmin::text, '-'),
                                  coalesce(f.raw_flags::text, '')),
                           E'\n' ORDER BY lp),
                '(no line pointers)')
FROM heap_page_items(get_raw_page('$rel', 0)) hpi
     LEFT JOIN LATERAL heap_tuple_infomask_flags(hpi.t_infomask, hpi.t_infomask2) f
       ON true]);
}

#
# Part 1: TOAST chunks pruned out from under a row that is then committed.
#

# The culprit stops inside its subtransaction, after the INSERT and before the
# subcommit, so that VACUUM's main-table pass can run while its XID is still
# simply in progress.
$node->safe_psql('postgres',
	q[SELECT injection_points_attach('subxact-insert-done', 'wait')]);

my $culprit = $node->background_psql('postgres', on_error_stop => 0);
$culprit->query_safe(q[BEGIN]);
$culprit->query_until(
	qr//, q[
DO $$
BEGIN
  BEGIN
    INSERT INTO audit_log VALUES (1, repeat('x', 7900));
    PERFORM injection_points_run('subxact-insert-done');
  EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'subxact aborted: %', SQLERRM;
  END;
END $$;
]);

$node->poll_query_until('postgres',
	q[SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'subxact-insert-done']
) or die "culprit never reached the injection point";

is( $node->safe_psql('postgres',
		qq[SELECT count(*) FROM heap_page_items(get_raw_page('$toastrel', 0)) WHERE lp_flags = 1]
	),
	'4',
	'the four TOAST chunks are on the page');

# VACUUM's main-table pass, with the subtransaction's XID still in progress.
# PROCESS_TOAST off stops it before the TOAST relation, which is where
# vacuum_rel() would go next, in its own transaction.
$node->safe_psql('postgres',
	q[VACUUM (DISABLE_PAGE_SKIPPING, PROCESS_TOAST FALSE) audit_log]);

diag("audit_log heap page after the main-table pass:\n" . heap_flags('audit_log'));
is( $node->safe_psql('postgres',
		q[SELECT count(*) FROM heap_page_items(get_raw_page('audit_log', 0)) WHERE lp_flags = 1]
	),
	'1',
	'the main-table pass leaves the in-progress tuple alone');

# Now open the window: let the subtransaction proceed into CommitSubTransaction()
# and throw once its XID has been handed to the parent.
$node->safe_psql('postgres',
	q[SELECT injection_points_attach('subxact-after-childxids-transfer', 'error')]);
$node->safe_psql('postgres',
	q[SELECT injection_points_wakeup('subxact-insert-done')]);

# Pump the culprit's output until the DO block has finished and the error has
# been swallowed by its EXCEPTION handler.
$culprit->query_until(qr/window-open/, qq[SELECT 'window-open';\n]);

my $subxact_err = $culprit->{stderr};
$culprit->{stderr} = '';
like(
	$subxact_err,
	qr/AbortSubTransaction while in COMMIT state/,
	'subtransaction aborted after it had already subcommitted');

# VACUUM's TOAST pass, which vacuum_rel() would have run next anyway.
$node->safe_psql('postgres', qq[VACUUM (DISABLE_PAGE_SKIPPING) $toastrel]);

diag("TOAST relation after the TOAST pass: "
	  . $node->safe_psql('postgres',
		qq[SELECT pg_relation_size('$toastrel') / 8192 || ' block(s), ' ||
		          (SELECT count(*) FROM $toastrel) || ' chunk(s)']));

# Committing the parent is what does the damage: its commit record carries the
# aborted subtransaction in the child list, so that XID is marked COMMITTED and
# the heap row becomes live -- pointing at chunks that are already gone.
$culprit->query_safe(q[COMMIT]);
$culprit->quit;

diag("audit_log heap page after the commit:\n" . heap_flags('audit_log'));

my ($rc, $stdout, $stderr) =
  $node->psql('postgres', 'SELECT id, length(payload) FROM audit_log');
diag("reading the row says: $stderr") if $stderr ne '';
unlike(
	$stderr,
	qr/missing chunk number/,
	'a visible row must not have lost its TOAST chunks');
is($rc, 0, 'the row is readable');

# Whatever pg_xact decided, the heap has to agree with it.
my $audit_status = $node->safe_psql('postgres', q[
SELECT DISTINCT pg_xact_status(t_xmin::text::xid8)
FROM heap_page_items(get_raw_page('audit_log', 0)) WHERE t_xmin IS NOT NULL]);
diag("pg_xact status of the inserting XID: $audit_status");
is( $node->safe_psql('postgres', 'SELECT count(*) FROM audit_log'),
	$audit_status eq 'committed' ? '1' : '0',
	'row visibility agrees with pg_xact');

diag("verify_heapam:\n"
	  . $node->safe_psql('postgres',
		q[SELECT coalesce(string_agg(format('blkno %s offnum %s attnum %s: %s',
		                                    blkno, offnum, attnum, msg), E'\n'), '(clean)')
		  FROM verify_heapam('audit_log', check_toast => true, skip => 'none')]));
is( $node->safe_psql('postgres',
		q[SELECT count(*) FROM verify_heapam('audit_log', check_toast => true, skip => 'none')]
	),
	'0',
	'verify_heapam must find no damage');

#
# Part 2: ordinary rows lost permanently.  No TOAST, no VACUUM, no pruning --
# just a reader that happened to look during the window.
#
my $culprit2 = $node->background_psql('postgres', on_error_stop => 0);
$culprit2->query_safe(q[BEGIN]);
$culprit2->query(q[
DO $$
BEGIN
  BEGIN
    INSERT INTO plain SELECT g, 'inserted by the subtransaction'
      FROM generate_series(1, 4) g;
  EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'subxact aborted: %', SQLERRM;
  END;
END $$;
]);
$culprit2->{stderr} = '';

# Correctly invisible here: the transaction that wrote them has not committed.
is($node->safe_psql('postgres', 'SELECT count(*) FROM plain'),
	'0', 'rows are not visible while the transaction is still open');

$culprit2->query_safe(q[COMMIT]);
$culprit2->quit;

diag("plain heap page after the commit:\n" . heap_flags('plain'));

my $plain_status = $node->safe_psql('postgres', q[
SELECT DISTINCT pg_xact_status(t_xmin::text::xid8)
FROM heap_page_items(get_raw_page('plain', 0)) WHERE t_xmin IS NOT NULL]);
diag("pg_xact status of the inserting XID: $plain_status");

my $expected = $plain_status eq 'committed' ? '4' : '0';
is($node->safe_psql('postgres', 'SELECT count(*) FROM plain'),
	$expected, 'row visibility agrees with pg_xact');

# The hint bits are only in shared buffers so far.  Flush them and bounce the
# server to show the verdict is on disk and permanent.
$node->safe_psql('postgres', 'CHECKPOINT');
$node->restart;

is($node->safe_psql('postgres', 'SELECT count(*) FROM plain'),
	$expected, 'and still agrees after a checkpoint and a restart');

done_testing();
