# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that redo of a btree page unlink recreates the half-dead leaf page with
# its current left sibling link.
#
# When VACUUM deletes a subtree (an internal page plus its only remaining leaf
# child), the UNLINK_PAGE record for the internal page also carries the leaf's
# sibling links, since redo rebuilds the leaf page from scratch.
# _bt_unlink_halfdead_page() used to read those links before it released its
# lock on the leaf, and did not read them again once it relocked the leaf to
# write the record.  A concurrent split of the leaf's left sibling in that
# window advanced the leaf's left link on the primary, but the stale link was
# what got logged.  Replay then built a leaf page whose left link skipped the
# new sibling, so a standby (or the primary after crash recovery) disagreed
# with the primary's own copy of the page.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

if (!$primary->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$primary->safe_psql('postgres', 'CREATE EXTENSION pageinspect');

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->start;

# A three-level index whose layout is fixed by its key width: each leaf page
# holds two keys, and each internal page up to three downlinks.  (STORAGE
# PLAIN keeps the index from compressing the keys.)
#
#                               root
#                  /              |               \
#               [X]              [P]              [Z]
#             /  |  \          /  |  \            /  \
#           [A] [B] [C]     [D] [E] [LEAF]     [G]  [H]
#           10  30  50      70  90  110        130  150
#           20  40  60      80 100  120        140  160
$primary->safe_psql(
	'postgres', q{
CREATE TABLE t (i int, k text STORAGE PLAIN) WITH (autovacuum_enabled = off);
INSERT INTO t SELECT i, lpad(i::text, 2600, '0') FROM generate_series(10, 160, 10) i;
CREATE INDEX t_k ON t (k);
});

# P is the root's second downlink, and LEAF is P's last downlink
my $root = $primary->safe_psql('postgres', "SELECT root FROM bt_metap('t_k')");
my $p = $primary->safe_psql('postgres',
	"SELECT (ctid::text::point)[0]::int FROM bt_page_items('t_k', $root) WHERE itemoffset = 2"
);
my $leaf = $primary->safe_psql('postgres',
	"SELECT (ctid::text::point)[0]::int FROM bt_page_items('t_k', $p) ORDER BY itemoffset DESC LIMIT 1"
);
my $leftlink = "SELECT btpo_prev FROM bt_page_stats('t_k', $leaf)";

# The rest of the test relies on that layout
my $leafkeys = $primary->safe_psql('postgres',
	"SELECT string_agg(i::text, ' ' ORDER BY i) FROM t WHERE ctid IN (SELECT htid FROM bt_page_items('t_k', $leaf))"
);
is($leafkeys, '110 120', 'LEAF holds keys 110 and 120');

# Delete D and E, which leaves LEAF as P's only child, and then empty LEAF too.
# The next VACUUM deletes the {P, LEAF} subtree: it unlinks P first, then LEAF.
$primary->safe_psql(
	'postgres', q{
DELETE FROM t WHERE i IN (70, 80, 90, 100);
VACUUM t;
DELETE FROM t WHERE i IN (110, 120);
});

# Run that VACUUM in the background, and stop it once it has read LEAF's
# sibling links and dropped its lock on LEAF, just before it unlinks P.
my $vacuum = $primary->background_psql('postgres');
$vacuum->query_safe(
	q{
SELECT injection_points_set_local();
SELECT injection_points_attach('nbtree-leave-page-half-dead', 'wait');
});
$vacuum->query_until(qr//, "VACUUM t;\n");
$primary->wait_for_event('client backend', 'nbtree-leave-page-half-dead');

# Split C, LEAF's left sibling, which advances LEAF's left link to the new page
my $before = $primary->safe_psql('postgres', $leftlink);
$primary->safe_psql('postgres',
	"INSERT INTO t VALUES (65, lpad('65', 2600, '0'))");
my $after = $primary->safe_psql('postgres', $leftlink);
isnt($after, $before, "left sibling split while VACUUM held no lock on LEAF");

# Let VACUUM unlink P, whose UNLINK_PAGE record carries LEAF's left link, and
# stop it again before it unlinks LEAF itself
$primary->safe_psql('postgres',
	"SELECT injection_points_wakeup('nbtree-leave-page-half-dead')");
$primary->poll_query_until('postgres',
	"SELECT type = 'D' FROM bt_page_stats('t_k', $p)")
  or die "VACUUM did not unlink P";
$primary->wait_for_event('client backend', 'nbtree-leave-page-half-dead');

# The record is not flushed before VACUUM commits, so force it out to the
# standby, which recreates LEAF when it replays the record
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$primary->wait_for_catchup($standby);

my $primary_leftlink = $primary->safe_psql('postgres', $leftlink);
my $standby_leftlink = $standby->safe_psql('postgres', $leftlink);
note
  "LEAF left link: $before before the split, $after after; primary=$primary_leftlink standby=$standby_leftlink";
is($standby_leftlink, $primary_leftlink,
	'replayed LEAF page has the same left link as on the primary');

# Let VACUUM finish
$primary->safe_psql('postgres',
	"SELECT injection_points_wakeup('nbtree-leave-page-half-dead')");
$vacuum->quit;

done_testing();
