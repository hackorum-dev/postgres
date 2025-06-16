# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

my $port = $node->port;


# TODO test that --section=post --clean, does not clean any tables.

# Create database
$node->safe_psql('postgres','
			CREATE DATABASE db1;
			\c db1
			CREATE TABLE t1 (
				i  integer
			);
			INSERT INTO t1 VALUES (1), (2), (3), (4);
			CREATE TABLE t2 (
				t  text
			);
			INSERT INTO t2 VALUES (\'a\'), (\'bb\'), (\'ccc\');');

# Function to compare two databases of the above kind.
sub compare_db_contents
{
	my ($db1, $db2, $should_match) = @_;
	$should_match = 1
	  unless defined $should_match;

	my $query = "
		SELECT * FROM t1 ORDER BY i;
		SELECT * FROM t2 ORDER BY t;
	";
	my $result1 = $node->safe_psql($db1, $query);
	my $result2 = $node->safe_psql($db2, $query);

	if ($should_match)
	{
		is($result2, $result1, "The database contents should match");
	} else {
		isnt($result2, $result1, "The database contents should NOT match");
	}
}

sub test_pg_restore
{
	my $dump_file = shift;
	my $file_basename = File::Basename::basename($dump_file);

	my @cmd = ( 'pg_restore', '--dbname', 'db1_restored' );
	my $cmd_s = "pg_restore";

	# Optionally this function takes a hash as last parameter.
	if ($_[0])
	{
		my (%hash_args) = @_;
		shift;
		my @extra_args = $hash_args{'extra_args'};
		if (@extra_args)
		{
			@cmd   = (@cmd, @extra_args);
			$cmd_s = "$cmd_s @extra_args";
		}
	}

	$node->safe_psql(
		'postgres',
		'DROP DATABASE IF EXISTS db1_restored;');
	ok(1, "clean up");

	# Restore into a new database
	$node->safe_psql('postgres',
					 'CREATE DATABASE db1_restored;');
	$node->command_ok([@cmd,
					   $dump_file],
					  "$cmd_s  $file_basename");

	# Verify restored db matches the dumped one
	compare_db_contents('db1', 'db1_restored');

	# Restore again with --data-only.
	# Now the rows should be duplicate, the databases shouldn't match.
	$node->command_ok([@cmd, '--data-only',
					   $dump_file],
					  "$cmd_s --data-only  $file_basename");
	compare_db_contents('db1', 'db1_restored', 0);

	# Restore again with --data-only --clean.
	# The database contents should match.
	$node->command_ok([@cmd, '--clean', '--data-only',
					   $dump_file],
					  "$cmd_s --clean --data-only  $file_basename");
	compare_db_contents('db1', 'db1_restored');

	# Restore from stdin.
	my $stderr;
	my $result = $node->run_log([@cmd, '--clean', '--data-only'],
								('<'  => $dump_file,
								 '2>' => \$stderr));
	if (grep {/^-j/} @cmd)
	{
		ok(!$result, "should fail:  $cmd_s --clean --data-only  < $file_basename");
		chomp($stderr);
		like($stderr,
		   '/parallel restore from standard input is not supported$/',
		   "stderr: parallel restore from standard input is not supported");
	}
	else
	{
		ok($result, "$cmd_s --clean --data-only  < $file_basename");
		compare_db_contents('db1', 'db1_restored');
	}
}


# Basic dump
my $d1 = "$tempdir/dump_file";
$node->command_ok(['pg_dump', '--format=custom',
				   '--file', $d1, 'db1'],
				  'pg_dump --format=custom --file dump_file');
# Dump also to stdout, as the TOC doesn't contain offsets.
my $d2 = "$tempdir/dump_file_stdout";
my $result = $node->run_log(['pg_dump', '--format=custom', 'db1'],
							('>' => $d2));
ok($result, "pg_dump --format=custom      > dump_file_stdout");


# Run all pg_restore testcases against each archive.
test_pg_restore($d1);
test_pg_restore($d2);
test_pg_restore($d1, ('extra_args' => ('-j2')));
test_pg_restore($d2, ('extra_args' => ('-j2')));
test_pg_restore($d1, ('extra_args' => ('--single-transaction')));
test_pg_restore($d2, ('extra_args' => ('--single-transaction')));


$node->stop('fast');

done_testing();
