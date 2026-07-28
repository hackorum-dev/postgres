# Copyright (c) 2026, PostgreSQL Global Development Group

# Expect XLogReader to reject multi-page records with xl_tot_len near
# UINT32_MAX (or above XLogRecordMaxSize) without crashing.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

my $exe = 'test_xlogreader_oversized';

note "running $exe (expects clean rejection of oversized multi-page WAL)";

# With the allocate_recordbuf overflow bug this often dies on SIGSEGV.
# After a proper max-length check it exits 0 and prints OK.
my $result = IPC::Run::run [ $exe ], '>', \my $stdout, '2>', \my $stderr;
my $exit = $? >> 8;
my $signal = $? & 127;

if ($signal)
{
	fail("test_xlogreader_oversized died with signal $signal (likely heap overflow in allocate_recordbuf / reassembly)");
	diag("stdout: $stdout") if length $stdout;
	diag("stderr: $stderr") if length $stderr;
}
else
{
	is($exit, 0, 'test_xlogreader_oversized exits 0');
	like($stdout, qr/^OK:/, 'reports clean rejection on stdout');
	is($stderr, '', 'no stderr on success');
}

done_testing();
