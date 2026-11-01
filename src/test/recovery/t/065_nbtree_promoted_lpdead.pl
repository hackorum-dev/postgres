# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that simple index deletion on a promoted standby does not remove an
# index entry that a transaction which began during recovery still needs.
#
# The primary sets LP_DEAD bits by its own xmin horizon, and full-page images
# carry them to the standby.  A transaction that began during recovery ignores
# those bits in its own scans, and it survives promotion.  After promotion it
# must still find, through the index, every row that its snapshot can see.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'autovacuum = off');
$primary->start;

$primary->safe_psql(
	'postgres', q(
	CREATE EXTENSION pageinspect;
	CREATE TABLE lpdead_victim (id int, k int, pad text);
	CREATE INDEX lpdead_victim_k ON lpdead_victim (k);
	INSERT INTO lpdead_victim SELECT i, i % 10, 'x' FROM generate_series(1, 100) i;
	VACUUM lpdead_victim;
));

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->start;
$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));

# A transaction that begins during recovery, and sees all ten k = 1 rows.
my $rr = $standby->background_psql('postgres', on_error_stop => 1);
$rr->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
is(count_k1($rr, 'index'), 10,
	'snapshot taken during recovery sees ten rows');

# Delete one of them on the primary, and let index scans set its LP_DEAD bit.
$primary->safe_psql('postgres', 'DELETE FROM lpdead_victim WHERE id = 11');
for (1 .. 3)
{
	$primary->safe_psql(
		'postgres', q(
		SET enable_seqscan = off;
		SET enable_bitmapscan = off;
		SELECT count(*) FROM lpdead_victim WHERE k = 1;
	));
}
is(dead_victim($primary), 1, 'primary marked the deleted row LP_DEAD');

# The first change after a checkpoint ships the leaf page, bit included, as a
# full-page image.
$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->safe_psql('postgres',
	q(INSERT INTO lpdead_victim VALUES (1000, 5, 'y')));
$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));
is(dead_victim($standby), 1, 'the LP_DEAD bit reached the standby');

$standby->promote;

# A new inserter on the promoted server fills the leaf page, so simple
# deletion runs over the inherited LP_DEAD item.
$standby->safe_psql('postgres',
	q(INSERT INTO lpdead_victim SELECT i, i % 9 + 2, 'z' FROM generate_series(2000, 2399) i)
);

is(count_k1($rr, 'seq'), 10, 'snapshot still sees ten rows by seqscan');
is(count_k1($rr, 'index'), 10, 'snapshot still sees ten rows by index scan');
$rr->query_safe('COMMIT');
$rr->quit;

done_testing();

sub count_k1
{
	my ($session, $how) = @_;
	my ($seqscan, $indexscan) = $how eq 'index' ? ('off', 'on') : ('on', 'off');
	my $sql =
	    "SET LOCAL enable_seqscan = $seqscan; SET LOCAL enable_bitmapscan = off; "
	  . "SET LOCAL enable_indexscan = $indexscan; SET LOCAL enable_indexonlyscan = $indexscan;";
	return $session->query_safe(
		"$sql SELECT count(*) FROM lpdead_victim WHERE k = 1 AND id <= 100");
}

sub dead_victim
{
	my ($node) = @_;
	return $node->safe_psql('postgres',
		q(SELECT count(*) FROM bt_page_items('lpdead_victim_k', 1) WHERE dead AND ctid = '(0,11)')
	);
}
