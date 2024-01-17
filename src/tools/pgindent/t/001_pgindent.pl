# Copyright (c) 2024, PostgreSQL Global Development Group
#
# Check that all C code is formatted with pgindent
#

use strict;
use warnings FATAL => 'all';
use Test::More;
use PostgreSQL::Test::Utils qw(command_ok);

if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bpgindent\b/)
{
	plan skip_all => "test pgindent not enabled in PG_TEST_EXTRA";
}

my $pg_bsd_indent = $ARGV[0];
command_ok(["./pgindent", "--indent=$pg_bsd_indent", "--check", "--diff"]);

done_testing();
