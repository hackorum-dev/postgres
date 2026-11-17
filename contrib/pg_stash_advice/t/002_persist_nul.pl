# Copyright (c) 2016-2026, PostgreSQL Global Development Group

# Test that pg_stash_advice treats an embedded NUL byte in its dump file as
# corruption, rather than as an end-of-file marker.  Previously, the part of
# the file after the NUL was silently ignored, the prefix was accepted as the
# complete snapshot, and the next write replaced the original file with it.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');

$node->init;
$node->append_conf(
	'postgresql.conf',
	qq{shared_preload_libraries = 'pg_plan_advice, pg_stash_advice'
pg_stash_advice.persist = true
pg_stash_advice.persist_interval = 1});

my $dumpfile = $node->data_dir . '/pg_stash_advice.tsv';
my $logfile = $node->logfile;

sub write_binary
{
	my ($path, $bytes) = @_;
	open(my $fh, '>:raw', $path) or die "could not open $path: $!";
	print $fh $bytes;
	close($fh) or die "could not close $path: $!";
}

sub read_binary
{
	my ($path) = @_;
	open(my $fh, '<:raw', $path) or die "could not open $path: $!";
	local $/;
	my $bytes = <$fh>;
	close($fh);
	return defined $bytes ? $bytes : '';
}

# Install $bytes as the dump file, start the server, and wait until the
# persistence worker has either loaded the file or complained about it.
# Returns the portion of the server log generated since the start.
sub start_with_dump_file
{
	my ($bytes) = @_;

	write_binary($dumpfile, $bytes);
	my $offset = -s $logfile;
	$node->start;
	$node->wait_for_log(
		qr/loaded \d+ advice stashes and \d+ entries|syntax error in file/,
		$offset);
	return slurp_file($logfile, $offset);
}

# Install a corrupt dump file and verify that it is reported, not loaded, not
# usable as a partial snapshot, and not overwritten.
sub test_nul_rejected
{
	my ($name, $bytes, $line, $offset) = @_;

	my $log = start_with_dump_file($bytes);

	like(
		$log,
		qr/ERROR:  syntax error in file "pg_stash_advice.tsv" line $line: unexpected NUL byte at offset $offset\b/,
		"$name: NUL byte reported as corruption");
	unlike($log, qr/loaded \d+ advice stashes/,
		"$name: file not reported as loaded");

	# Nothing from the file may be visible, and modifications must be locked
	# out, so that no partial state can be saved over the original file.
	is( $node->safe_psql(
			"postgres", "SELECT count(*) FROM pg_get_advice_stashes()"),
		"0",
		"$name: no stashes visible");
	my ($ret, $stdout, $stderr) =
	  $node->psql("postgres", "SELECT pg_create_advice_stash('probe')");
	isnt($ret, 0, "$name: stash creation refused");
	like(
		$stderr,
		qr/stash modifications are not allowed because "pg_stash_advice.tsv" has not been loaded yet/,
		"$name: stash modifications locked out");

	# A clean shutdown must not rewrite the damaged file either.
	$node->stop;
	ok(-f $dumpfile, "$name: dump file still exists after shutdown");
	ok(read_binary($dumpfile) eq $bytes,
		"$name: dump file byte-for-byte unchanged after shutdown");
}

# Create the extension once; it persists across the restarts below.
$node->start;
$node->safe_psql("postgres", "CREATE EXTENSION pg_stash_advice;\n");
$node->stop;
# The empty dump file written at that last shutdown, if any, is replaced by
# each test case below.
unlink($dumpfile);

my $head = "stash\tbefore\nentry\tbefore\t1001\tSEQ_SCAN(t1)\n";
my $tail = "stash\tafter\nentry\tafter\t1002\tSEQ_SCAN(t2)\n";

# Controls: a valid file with multiple stashes and entries is loaded in full.
{
	my $valid =
	  $head . "entry\tbefore\t1003\tHASH_JOIN(t1)\n" . $tail . "stash\tempty\n";
	my $log = start_with_dump_file($valid);
	like($log, qr/loaded 3 advice stashes and 3 entries/,
		'valid file: loaded in full');
	is( $node->safe_psql(
			"postgres",
			"SELECT stash_name, num_entries FROM pg_get_advice_stashes() ORDER BY 1"
		),
		"after|1\nbefore|2\nempty|0",
		'valid file: stashes present');
	$node->stop;

	# The shutdown rewrite must preserve everything.
	is( join('', sort(split(/^/m, read_binary($dumpfile)))),
		join('', sort(split(/^/m, $valid))),
		'valid file: contents preserved by rewrite');
}

# Control: a valid file without a final newline is still accepted.
{
	my $log = start_with_dump_file($head . "stash\tlast");
	like($log, qr/loaded 2 advice stashes and 1 entries/,
		'no final newline: loaded');
	$node->stop;
}

# Control: an empty file is accepted and means "no stashes".
{
	my $log = start_with_dump_file('');
	like($log, qr/loaded 0 advice stashes and 0 entries/,
		'empty file: loaded as empty');
	$node->poll_query_until("postgres",
		"SELECT pg_create_advice_stash('probe')", '')
	  or die "timed out waiting for pg_stash_advice to load";
	$node->safe_psql("postgres", "SELECT pg_drop_advice_stash('probe')");
	$node->stop;
}

# Control: ordinary malformed content is still reported as before.
{
	my $log = start_with_dump_file($head . "bogus\tline\n");
	like(
		$log,
		qr/syntax error in file "pg_stash_advice.tsv" line 3: unrecognized line type/,
		'ordinary malformed record still reported');
	$node->stop;
}

# The scenario from the bug report: valid records before and after a NUL.
test_nul_rejected('NUL between records',
	$head . "\0\n" . $tail,
	3, length($head));

# NUL at the very beginning of the file.
test_nul_rejected('NUL at start', "\0" . $head . $tail, 1, 0);

# NUL on a line by itself, directly following a record's newline, with the
# rest of the file consisting of a single valid record.
test_nul_rejected('NUL after first record',
	"stash\tbefore\n\0entry\tbefore\t1001\tSEQ_SCAN(t1)\n",
	2, length("stash\tbefore\n"));

# NUL inside an advice string.
{
	my $prefix = "stash\tbefore\nentry\tbefore\t1001\tSEQ_";
	test_nul_rejected('NUL inside advice string',
		$prefix . "\0SCAN(t1)\n" . $tail,
		2, length($prefix));
}

# NUL as the final byte before physical EOF.
test_nul_rejected('NUL as last byte', $head . $tail . "\0",
	5, length($head . $tail));

done_testing();
