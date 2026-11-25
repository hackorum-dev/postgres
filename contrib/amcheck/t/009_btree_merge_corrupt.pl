
# Copyright (c) 2026, PostgreSQL Global Development Group

#
# Tests that amcheck detects corrupted merge groups: a tombstone
# (BTP_MERGED_AWAY page) and the merged pages (BTP_MERGED) that name it.
# Each case restores clean copies of the indexes, changes a few bytes on
# disk and runs the checks that must report it, in the style of
# 001_verify_heapam.pl.
#
use strict;
use warnings FATAL => 'all';
use File::Copy;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');

# Checksums are off, so a changed page can still be read.
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql('postgres',
	'CREATE EXTENSION amcheck; CREATE EXTENSION pageinspect');

my $blcksz =
  $node->safe_psql('postgres', "SELECT current_setting('block_size')");

# Byte offsets in a page.  pd_prune_xid holds a merged page's tombstone
# block.  In a tombstone, the boundary size is at 32.
my $off_prune_xid = 20;
my $off_boundary_size = 32;
my $off_btpo_prev = $blcksz - 16;
my $off_btpo_flags = $blcksz - 4;

my $BTP_MERGED = 1 << 9;
my $BTP_MERGED_AWAY = 1 << 10;

# Returns the file of an index.
sub relpath
{
	my ($index) = @_;

	return $node->data_dir . '/'
	  . $node->safe_psql('postgres',
		"SELECT pg_relation_filepath('$index')");
}

# Returns every leaf page of an index with its links, flags and pd_prune_xid,
# keyed by block number.
sub leaf_pages
{
	my ($index) = @_;
	my %page;

	foreach my $row (
		split /\n/,
		$node->safe_psql(
			'postgres', qq{
		SELECT s.blkno, s.btpo_prev, s.btpo_next, s.btpo_flags, h.prune_xid
		FROM bt_multi_page_stats('$index', 1, -1) s,
			LATERAL page_header(get_raw_page('$index', s.blkno::int)) h
		WHERE s.btpo_level = 0
	}))
	{
		my ($blkno, $prev, $next, $flags, $prune) = split /\|/, $row;
		$page{$blkno} =
		  { prev => $prev, next => $next, flags => $flags, prune => $prune };
	}

	return %page;
}

# True when a leaf page is neither merged nor a tombstone.
sub is_plain
{
	my ($page) = @_;

	return defined $page
	  && ($page->{flags} & ($BTP_MERGED | $BTP_MERGED_AWAY)) == 0;
}

#
# Index t_idx.  The first insert splits one merged page into three, so one
# group has several merged pages.  The second fills the merged page of
# another group.
#
$node->safe_psql(
	'postgres', q{
	CREATE TABLE t (id int, pad text) WITH (autovacuum_enabled = false);
	INSERT INTO t SELECT i, 'x' FROM generate_series(1, 10000) i;
	CREATE INDEX t_idx ON t (id);
	DELETE FROM t WHERE id % 20 != 0;
	VACUUM t;
	SELECT bt_merge('t_idx', 50, 90, 1000);
	INSERT INTO t SELECT i, 'x' FROM generate_series(381, 1098) i
		WHERE i % 20 != 0;
	INSERT INTO t SELECT i, 'x' FROM generate_series(1102, 1830, 2) i
		WHERE i % 20 != 0;
});

my %page = leaf_pages('t_idx');

# Each tombstone's group: the merged pages right of it that name it.
my %group;
foreach my $ma (grep { $page{$_}{flags} & $BTP_MERGED_AWAY } keys %page)
{
	my @members;
	my $blkno = $page{$ma}{next};
	while ($blkno != 0
		&& ($page{$blkno}{flags} & $BTP_MERGED)
		&& $page{$blkno}{prune} == $ma)
	{
		push @members, $blkno;
		$blkno = $page{$blkno}{next};
	}
	$group{$ma} = \@members;
}

my @multi = sort { $a <=> $b } grep { @{ $group{$_} } >= 2 } keys %group;
my @single = sort { $a <=> $b } grep { @{ $group{$_} } == 1 } keys %group;
my ($plain) = sort { $a <=> $b } grep { is_plain($page{$_}) } keys %page;
my $root = $node->safe_psql('postgres', "SELECT root FROM bt_metap('t_idx')");

ok(@multi >= 1, 'a merge group has several merged pages');
ok(@single >= 2, 'two merge groups have one merged page');
ok(defined $plain, 'a leaf page is neither merged nor a tombstone');

my $ma_multi = $multi[0];
my $m_multi = $group{$ma_multi}[0];
my ($ma_single, $ma_other) = @single;
my $m_single = $group{$ma_single}[0];

#
# Index u_idx, unique.  checkunique compares the last key of a page with the
# first key of the next live page.  The last case makes the first key of a
# merged page equal to the last key of its tombstone's left sibling, so the
# equal keys have a tombstone between them.  Both heap rows are visible, so
# this breaks uniqueness.
#
$node->safe_psql(
	'postgres', q{
	CREATE TABLE u (id int) WITH (autovacuum_enabled = false);
	INSERT INTO u SELECT generate_series(1, 10000);
	CREATE UNIQUE INDEX u_idx ON u (id);
	DELETE FROM u WHERE id % 20 != 0;
	VACUUM u;
	SELECT bt_merge('u_idx', 50, 90, 1000);
});

my %upage = leaf_pages('u_idx');

# A tombstone whose left sibling is a plain leaf page
my ($u_ma) = sort { $a <=> $b } grep {
	($upage{$_}{flags} & $BTP_MERGED_AWAY)
	  && is_plain($upage{ $upage{$_}{prev} })
} keys %upage;
ok(defined $u_ma, 'a tombstone has a plain left sibling');

my $u_left = $upage{$u_ma}{prev};
my $u_merged = $upage{$u_ma}{next};

# The key of the left sibling's last item and of the merged page's first
# item (offset 2, after the high key), found through their heap rows.
my $left_key = $node->safe_psql(
	'postgres', qq{
	SELECT id FROM u WHERE ctid =
		(SELECT htid FROM bt_page_items('u_idx', $u_left)
		 ORDER BY itemoffset DESC LIMIT 1)
});
my $merged_key = $node->safe_psql(
	'postgres', qq{
	SELECT id FROM u WHERE ctid =
		(SELECT htid FROM bt_page_items('u_idx', $u_merged)
		 WHERE itemoffset = 2)
});

# The int4 key follows the 8 byte index tuple header.
my $key_off = 8 + $node->safe_psql(
	'postgres', qq{
	SELECT lp_off FROM heap_page_items(get_raw_page('u_idx', $u_merged))
	WHERE lp = 2
});

# Save clean copies of both index files.
my %relpath = map { $_ => relpath($_) } qw(t_idx u_idx);

$node->stop;
foreach my $path (values %relpath)
{
	copy($path, "$path.clean") or BAIL_OUT("copy failed: $!");
}
$node->start;

# Write $new at $offset of block $blkno of an index, in pack() format $fmt.
# Refuses to write unless the old value equals $expected.
sub patch
{
	my ($index, $blkno, $offset, $fmt, $new, $expected) = @_;
	my $len = length(pack($fmt, 0));
	my ($fh, $buf);

	open($fh, '+<', $relpath{$index}) or BAIL_OUT("open failed: $!");
	binmode $fh;
	sysseek($fh, $blkno * $blcksz + $offset, 0)
	  or BAIL_OUT("sysseek failed: $!");
	sysread($fh, $buf, $len) == $len or BAIL_OUT("sysread failed: $!");
	my $old = unpack($fmt, $buf);
	BAIL_OUT("block $blkno offset $offset: expected $expected, found $old")
	  if $old != $expected;
	sysseek($fh, $blkno * $blcksz + $offset, 0)
	  or BAIL_OUT("sysseek failed: $!");
	syswrite($fh, pack($fmt, $new)) == $len
	  or BAIL_OUT("syswrite failed: $!");
	close($fh) or BAIL_OUT("close failed: $!");

	return;
}

my %check_sql = (
	bt_index_check => q{SELECT bt_index_check('t_idx', true)},
	bt_index_parent_check =>
	  q{SELECT bt_index_parent_check('t_idx', true, true)},
	checkunique => q{SELECT bt_index_check('u_idx', false, true)});

foreach my $fn (sort keys %check_sql)
{
	my ($ret, $stdout, $stderr) = $node->psql('postgres', $check_sql{$fn});
	is($ret, 0, "$fn passes on a clean index") or diag($stderr);
}

#
# For each case, "checks" lists the checks that must report it.  The group
# walk runs only in bt_index_parent_check, where no VACUUM can run.
#
my @cases = (
	{
		name => 'merged page names a plain leaf page',
		index => 't_idx',
		patch => sub {
			patch('t_idx', $m_single, $off_prune_xid, 'L', $plain, $ma_single);
		},
		checks => [ 'bt_index_check', 'bt_index_parent_check' ],
	},
	{
		name => 'first merged page of a group lost its merged flag',
		index => 't_idx',
		patch => sub {
			patch('t_idx', $m_multi, $off_btpo_flags, 'S',
				$page{$m_multi}{flags} & ~$BTP_MERGED,
				$page{$m_multi}{flags});
			patch('t_idx', $m_multi, $off_prune_xid, 'L', 0, $ma_multi);
		},
		checks => ['bt_index_parent_check'],
	},
	{
		name => 'merged page names the tombstone of another group',
		index => 't_idx',
		patch => sub {
			patch('t_idx', $m_single, $off_prune_xid, 'L', $ma_other,
				$ma_single);
		},
		checks => ['bt_index_parent_check'],
	},
	{
		# An int4 boundary is 8 bytes of header, 4 of key and 4 of padding,
		# then the 8 byte heap TID.
		name => 'boundary key size does not match the tuple',
		index => 't_idx',
		patch => sub {
			patch('t_idx', $ma_multi, $off_boundary_size, 'S', 32, 24);
		},
		checks => [ 'bt_index_check', 'bt_index_parent_check' ],
	},
	{
		name => 'tombstone\'s left link points to the root',
		index => 't_idx',
		patch => sub {
			patch('t_idx', $ma_multi, $off_btpo_prev, 'L', $root,
				$page{$ma_multi}{prev});
		},
		checks => [ 'bt_index_check', 'bt_index_parent_check' ],
	},
	{
		name => 'equal keys on both sides of a tombstone',
		index => 'u_idx',
		patch => sub {
			patch('u_idx', $u_merged, $key_off, 'l', $left_key, $merged_key);
		},
		checks => ['checkunique'],
		error => qr/index uniqueness is violated for index "u_idx"/,
	},);

foreach my $case (@cases)
{
	my $error = $case->{error} // qr/ERROR: .*index "$case->{index}"/;

	$node->stop;
	foreach my $path (values %relpath)
	{
		copy("$path.clean", $path) or BAIL_OUT("copy failed: $!");
	}
	$case->{patch}->();
	$node->start;

	foreach my $fn (@{ $case->{checks} })
	{
		my ($ret, $stdout, $stderr) =
		  $node->psql('postgres', $check_sql{$fn});

		like($stderr, $error, "$fn detects: $case->{name}");
	}
}

$node->stop;

done_testing();
