# Copyright (c) 2025, PostgreSQL Global Development Group

# Tests for transfer pg_commit_ts directory.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub command_output
{
	my ($cmd) = @_;
	my ($stdout, $stderr);

	print("# Running: " . join(" ", @{$cmd}) . "\n");

	my $result = IPC::Run::run $cmd, '>', \$stdout, '2>', \$stderr;
	ok($result, "@$cmd exit code 0");
	is($stderr, '', "@$cmd no stderr");

	return $stdout;
}

# Can be changed to test the other modes
my $mode = $ENV{PG_TEST_PG_UPGRADE_MODE} || '--copy';

# Initialize old cluster
my $old = PostgreSQL::Test::Cluster->new('old');
$old->init;
$old->append_conf('postgresql.conf', 'track_commit_timestamp = on');
$old->start;
$old->command_ok([ 'pgbench', '-i', '-s', '1' ], 'init pgbench');
$old->command_ok([ 'pgbench', '-t', '100', '-j', '2' ], 'pgbench it');
$old->stop;

# Initialize new cluster
my $new = PostgreSQL::Test::Cluster->new('new');
$new->init;
$new->append_conf('postgresql.conf', 'track_commit_timestamp = on');

command_ok(
	[
		'pg_upgrade', '--no-sync',
		'-d', $old->data_dir,
		'-D', $new->data_dir,
		'-b', $old->config_data('--bindir'),
		'-B', $new->config_data('--bindir'),
		'-s', $new->host,
		'-p', $old->port,
		'-P', $new->port,
		$mode
	],
	'run of pg_upgrade');

$new->start;
$new->command_ok([ 'pgbench', '-t', '10' ], 'pgbench it');
$new->stop;

sub xact_commit_ts
{
	my ($node) = @_;
	my ($oldest, $newest);
	my $out = command_output([ 'pg_controldata', '-D', $node->data_dir ]);

	if ($out =~ /oldestCommitTsXid:(\d+)/) {
		$oldest = $1;
	}
	if ($out =~ /newestCommitTsXid:(\d+)/) {
		$newest= $1;
	}

	for my $line (grep { /\S/ } split /\n/, $out) {
		if ($line =~ /Latest checkpoint's NextXID:/) {
			print $line . "\n";
		}
		if ($line =~ /Latest checkpoint's oldestXID:/) {
			print $line . "\n";
		}
		if ($line =~ /Latest checkpoint's oldestCommitTsXid/) {
			print $line . "\n";
		}
		if ($line =~ /Latest checkpoint's newestCommitTsXid:/) {
			print $line . "\n";
		}
	}

	$node->start;
	my $res = $node->safe_psql('postgres', "
	WITH xids AS (
		SELECT v::text::xid AS x
		FROM generate_series($oldest, $newest) v
	)
	SELECT x, pg_xact_commit_timestamp(x::text::xid)
	FROM xids;
	");
	$node->stop;
	return grep { /\S/ } split /\n/, $res;
}

my @a = xact_commit_ts($old);
my @b = xact_commit_ts($new);
my %h1 = map { $_ => 1 } @a;
my %h2 = map { $_ => 1 } @b;

#
# All timestamps from the old cluster should appear in the new one.
#
my $count = 0;
print "Commit timestamp only in old cluster:\n";
for my $line (@a) {
	unless ($h2{$line}) {
		print "$line\n";
		$count = $count + 1;
	}
}
ok($count == 0, "all the timestamp transferred successfully");

#
# The new cluster should contain timestamps created during the pg_upgrade and
# some more created by the pgbench.
#
print "\nCommit timestamp only in new cluster:\n";
for my $line (@b) {
	print "$line\n" unless $h1{$line};
}

done_testing();
