
# Copyright (c) 2022-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use File::Basename;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::RecursiveCopy;
use PostgreSQL::Test::Utils;
use Test::More;

my ($blocksize, $walfile_name);
my $node = PostgreSQL::Test::Cluster->new('main');
my $pgdata = $node->data_dir;

# Returns the filesystem path for the named relation.
sub relation_filepath
{
    my ($relname) = @_;

    my $rel    = $node->safe_psql('postgres',
        qq(SELECT pg_relation_filepath('$relname')));
    die "path not found for relation $relname" unless defined $rel;
    return "$pgdata/$rel";
}

# Function to extract the LSN from the given block structure
sub get_block_lsn
{
	my $path = shift;
	my $blocksize = shift;
	my $block;

	open my $fh, '<', $path or die "couldn't open file: $path\n";
	die "could not read block\n"
	  if $blocksize != read($fh, $block, $blocksize);
	my ($lsn_hi, $lsn_lo) = unpack('LL', $block);

	$lsn_hi = sprintf('%08X', $lsn_hi);
	$lsn_lo = sprintf('%08X', $lsn_lo);

	return ($lsn_hi, $lsn_lo);
}

$node->init;
$node->append_conf(
	'postgresql.conf', q{
wal_level = 'replica'
max_wal_senders = 4
});
$node->start;

# Generate data/WAL to examine that will have full pages in them.
$node->safe_psql(
	'postgres',
	"SELECT 'init' FROM pg_create_physical_replication_slot('regress_pg_waldump_slot', true, false);
CREATE TABLE test_table AS SELECT generate_series(1,100) a;
-- Force FPWs on the next writes.
CHECKPOINT;
UPDATE test_table SET a = a + 1 where a = 1;
CHECKPOINT;
");

($walfile_name, $blocksize) = split '\|' => $node->safe_psql('postgres',
	"SELECT pg_walfile_name(pg_switch_wal()), current_setting('block_size')");

# Get the relation node, etc for the new table
my $relation = $node->safe_psql(
	'postgres',
	q{SELECT format(
        '%s/%s/%s',
        CASE WHEN reltablespace = 0 THEN dattablespace ELSE reltablespace END,
        pg_database.oid,
        pg_relation_filenode(pg_class.oid))
    FROM pg_class, pg_database
    WHERE relname = 'test_table' AND
        datname = current_database()}
);

my $walfile = $node->data_dir . '/pg_wal/' . $walfile_name;
my $tmp_folder = PostgreSQL::Test::Utils::tempdir;

ok(-f $walfile, "Got a WAL file");

$node->command_ok(
	[
		'pg_waldump',
		'--quiet',
		'--save-fullpage' => "$tmp_folder/raw",
		'--relation' => $relation,
		$walfile
	],
	'pg_waldump with --save-fullpage runs');

# This regexp will match filenames formatted as:
# TLI-LSNh-LSNl.TBLSPCOID.DBOID.NODEOID.dd_fork with the components being:
# - Timeline ID in hex format.
# - WAL LSN in hex format, as two 8-character numbers.
# - Tablespace OID (0 for global).
# - Database OID.
# - Relfilenode.
# - Block number.
# - Fork this block came from (vm, init, fsm, or main).
my $file_re =
  qr/^[0-9A-F]{8}-([0-9A-F]{8})-([0-9A-F]{8})[.][0-9]+[.][0-9]+[.][0-9]+[.][0-9]+(?:_vm|_init|_fsm|_main)?$/;

my $file_count = 0;

# Verify filename format matches --save-fullpage.
for my $fullpath (glob "$tmp_folder/raw/*")
{
	my $file = File::Basename::basename($fullpath);

	like($file, $file_re, "verify filename format for file $file");
	$file_count++;

	my ($hi_lsn_fn, $lo_lsn_fn) = ($file =~ $file_re);
	my ($hi_lsn_bk, $lo_lsn_bk) = get_block_lsn($fullpath, $blocksize);

	# The LSN on the block comes before the file's LSN.
	ok( $hi_lsn_fn . $lo_lsn_fn ge $hi_lsn_bk . $lo_lsn_bk,
		"LSN stored in the file $hi_lsn_fn/$lo_lsn_fn precedes the one stored in the block $hi_lsn_bk/$lo_lsn_bk"
	);
}

ok($file_count > 0, 'verify that at least one block has been saved');

# Check that the pg_waldump saves FPI file.
my @fpi_files = glob "$tmp_folder/raw/*_main";
is(scalar(@fpi_files), 1, 'one FPI file was created');

# Now extract the block from the relation file and compare with the FPI
my $relpath = relation_filepath('test_table');
my $blk;
my $blkfpi;

my $frel;
my $blkfrel;
my $blkfpifh;
my $blkfpinolsn;

open($frel, '+<', $relpath)
  or BAIL_OUT("open failed: $!");

open($blkfrel, '+>', "$tmp_folder/test_table.blk0")
  or BAIL_OUT("open failed: $!");

open($blkfpifh, '+<', $fpi_files[0])
  or BAIL_OUT("open failed: $!");

open($blkfpinolsn, '+>', "$tmp_folder/fpinolsn")
  or BAIL_OUT("open failed: $!");

binmode $frel;
binmode $blkfrel;
binmode $blkfpifh;
binmode $blkfpinolsn;

# Extract the binary data without the LSN from the relation's block
sysseek($frel, 8, 0); #bypass the LSN
sysread($frel, $blk, 8184) or die "sysread failed: $!";
syswrite($blkfrel, $blk) or die "syswrite failed: $!";

# Extract the binary data without the LSN from the FPI
sysseek($blkfpifh, 8, 0); #bypass the LSN
sysread($blkfpifh, $blkfpi, 8184) or die "sysread failed: $!";
syswrite($blkfpinolsn, $blkfpi) or die "syswrite failed: $!";

close($frel)
  or BAIL_OUT("close failed: $!");

close($blkfrel)
  or BAIL_OUT("close failed: $!");

close($blkfpifh)
  or BAIL_OUT("close failed: $!");

close($blkfpinolsn)
  or BAIL_OUT("close failed: $!");

# Compare the blocks (LSN excluded)
command_ok(
    [ 'diff', $tmp_folder . '/fpinolsn', $tmp_folder . '/test_table.blk0' ],
    'compare both pages');

done_testing();
