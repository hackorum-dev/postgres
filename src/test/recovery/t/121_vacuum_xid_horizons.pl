use strict;
use warnings;
use Test::More;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

# Start a minimal cluster to exercise VACUUM and XID horizon behavior.
my $node = PostgreSQL::Test::Cluster->new('relhorizon');
$node->init;

# Keep autovacuum out of the way and force VACUUM VERBOSE messages to be in
# English and to go to stderr.
$node->append_conf('postgresql.conf', q{
  client_min_messages = info
  autovacuum = off
  lc_messages = 'C'
});
$node->start;

# Run VACUUM (VERBOSE) and return all of its output.
# VACUUM VERBOSE writes to stderr, so collect both stdout and stderr.
sub vacuum_verbose {
    my ($relname) = @_;
    my ($out, $err) = ('', '');
    $node->psql('postgres', "VACUUM (VERBOSE) $relname;", stdout => \$out, stderr => \$err);
    return $out . $err;
}

# Helper regexp: either VACUUM truncated some table or removed some tuples.
# We know which relation we vacuumed from the test context, so we do not insist
# on a specific table name in the message.
my $vac_removed_or_truncated_re =
  qr/(table "[^"]+": truncated \d+ to \d+ pages|tuples:\s+\d+\s+removed)/;

# Helper regexp: VACUUM reports dead tuples that are "not yet removable".
my $vac_not_yet_removable_re =
  qr/\b[1-9]\d*\s+are dead but not yet removable/;

# One "old" session kept open to emulate a long-running transaction that
# influences the global XID horizon.
my $sess_old = $node->background_psql('postgres');

# ============================================================
# T1: relation created after the long-running transaction;
# VACUUM on such a relation should not be constrained by it
# ============================================================
$sess_old->query('BEGIN;');
$sess_old->query('SELECT txid_current();');    # ensure the session has an XID

$node->safe_psql('postgres', q{
  DROP TABLE IF EXISTS t1;
  CREATE TABLE t1(id int);
  INSERT INTO t1 SELECT generate_series(1,1000);
  DELETE FROM t1;
});

my $vac_t1 = vacuum_verbose('t1');

# VACUUM should be able to either truncate the relation or remove dead tuples.
like(
    $vac_t1,
    $vac_removed_or_truncated_re,
    'T1: VACUUM on relation created after old transaction can remove dead tuples'
);

# There must be no "are dead but not yet removable" due to the old transaction.
unlike(
    $vac_t1,
    $vac_not_yet_removable_re,
    'T1: old long-running transaction does not retain dead tuples in new relation'
);

# Statistics should also report no remaining dead tuples.
my $dead_t1 = $node->safe_psql('postgres', q{
  SELECT n_dead_tup FROM pg_stat_all_tables WHERE relname = 't1';
});
chomp $dead_t1;
is($dead_t1, '0', 'T1: pg_stat_all_tables reports zero dead tuples');

# ============================================================
# T2: REPEATABLE READ transaction started after relation creation
# must retain dead tuples until it commits
# ============================================================

# Create the test relation.
$node->safe_psql('postgres', q{
  DROP TABLE IF EXISTS t2;
  CREATE TABLE t2(id int);
  INSERT INTO t2 SELECT generate_series(1,5);
});

# Start a separate REPEATABLE READ transaction and take a snapshot.
my $sess_rr = $node->background_psql('postgres');
$sess_rr->query('BEGIN ISOLATION LEVEL REPEATABLE READ;');
$sess_rr->query('SELECT * FROM t2;');  # take a snapshot

# Delete rows in the main session.
$node->safe_psql('postgres', q{
  DELETE FROM t2;
});

# While the REPEATABLE READ transaction is open, VACUUM should report
# dead tuples as "not yet removable".
my $vac_t2_hold = vacuum_verbose('t2');
like(
    $vac_t2_hold,
    $vac_not_yet_removable_re,
    'T2: REPEATABLE READ snapshot keeps dead tuples while transaction is open'
);

# Once the REPEATABLE READ transaction commits, VACUUM can remove them.
$sess_rr->query('COMMIT;');

my $vac_t2_after = vacuum_verbose('t2');
like(
    $vac_t2_after,
    $vac_removed_or_truncated_re,
    'T2: after REPEATABLE READ commit VACUUM can remove dead tuples'
);

# ============================================================
# T3: VACUUM FULL rewrite must preserve the "birth XID" so that
# subsequent VACUUM can still remove dead tuples correctly
# ============================================================

$node->safe_psql('postgres', q{
  DROP TABLE IF EXISTS t3;
  CREATE TABLE t3(id int);
  INSERT INTO t3 SELECT generate_series(1,10);
  DELETE FROM t3;
});

my $vac_t3_first = vacuum_verbose('t3');
like(
    $vac_t3_first,
    $vac_removed_or_truncated_re,
    'T3: initial VACUUM on rewritten candidate relation works'
);

# Rewrite the relation with VACUUM FULL, then create and delete tuples again.
$node->safe_psql('postgres', q{
  VACUUM FULL t3;
  INSERT INTO t3 SELECT generate_series(11,20);
  DELETE FROM t3;
});

my $vac_t3_second = vacuum_verbose('t3');
like(
    $vac_t3_second,
    $vac_removed_or_truncated_re,
    'T3: VACUUM after VACUUM FULL rewrite still removes dead tuples'
);

# ============================================================
# T4: partitioned table — a partition created before the long-running
# transaction must be held; a newer partition must not
# ============================================================

$node->safe_psql('postgres', q{
  DROP TABLE IF EXISTS p_parent CASCADE;
  CREATE TABLE p_parent(id int, payload text) PARTITION BY RANGE (id);
  CREATE TABLE p_child1 PARTITION OF p_parent FOR VALUES FROM (1) TO (1000);
});

# Restart the long-running transaction after the first partition exists,
# so that p_child1 is older than its XID.
$sess_old->query('COMMIT;');
$sess_old->query('BEGIN;');
$sess_old->query('SELECT txid_current();');

# Create a newer partition and populate/delete tuples in both partitions.
$node->safe_psql('postgres', q{
  CREATE TABLE p_child2 PARTITION OF p_parent FOR VALUES FROM (1000) TO (2000);
  INSERT INTO p_child1 SELECT generate_series(1,100), repeat('x',10);
  DELETE FROM p_child1;
  INSERT INTO p_child2 SELECT generate_series(1000,1100), repeat('x',10);
  DELETE FROM p_child2;
});

# The partition created after the long-running transaction should be fully cleaned.
my $vac_p2 = vacuum_verbose('p_child2');
like(
    $vac_p2,
    $vac_removed_or_truncated_re,
    'T4: partition created after old transaction can be vacuumed fully'
);

# The older partition should still retain dead tuples while the transaction is open.
my $vac_p1_held = vacuum_verbose('p_child1');
like(
    $vac_p1_held,
    $vac_not_yet_removable_re,
    'T4: partition created before old transaction retains dead tuples'
);

# After the long-running transaction commits, VACUUM can remove tuples
# from the older partition as well.
$sess_old->query('COMMIT;');

my $vac_p1_after = vacuum_verbose('p_child1');
like(
    $vac_p1_after,
    $vac_removed_or_truncated_re,
    'T4: after old transaction commits older partition can be vacuumed fully'
);

# Cleanup.
$sess_rr->quit if $sess_rr;
$sess_old->quit;
$node->stop;
done_testing();
