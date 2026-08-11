
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test that psql counts every COPY ... FROM STDIN in a query string when it has
# to scan the string itself (the -c / \gexec / forced-send paths).  A COPY that
# is not the first sub-command must still be recognized, otherwise psql treats
# the server's COPY_IN response as unexpected and aborts the connection.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE TABLE t (a int)');

# Run "psql -c $sql" with $stdin fed to psql's stdin (so COPY FROM STDIN reads
# it).  Returns (success, stdout, stderr).
sub psql_c_stdin
{
	my ($sql, $stdin) = @_;
	my ($stdout, $stderr) = ('', '');

	my $ret = IPC::Run::run(
		[
			'psql', '-X', '-v' => 'ON_ERROR_STOP=1',
			'-d' => $node->connstr('postgres'),
			'-c' => $sql
		],
		'<' => \$stdin,
		'>' => \$stdout,
		'2>' => \$stderr);

	return ($ret, $stdout, $stderr);
}

my @cases = (
	{
		name => 'single COPY FROM STDIN',
		sql => 'COPY t FROM STDIN',
		data => "10\n20\n\\.\n",
		rows => 2,
	},
	{
		name => 'COPY FROM STDIN after another command',
		sql => 'SELECT 1; COPY t FROM STDIN',
		data => "30\n40\n\\.\n",
		rows => 2,
	},
	{
		name => 'two COPY FROM STDIN in one string',
		sql => 'COPY t FROM STDIN; COPY t FROM STDIN',
		data => "50\n\\.\n60\n\\.\n",
		rows => 2,
	});

foreach my $c (@cases)
{
	$node->safe_psql('postgres', 'TRUNCATE t');

	my ($ok, $stdout, $stderr) = psql_c_stdin($c->{sql}, $c->{data});

	ok($ok, "$c->{name}: psql exits 0");
	unlike($stderr, qr/unexpected COPY_IN result/,
		"$c->{name}: connection not aborted");

	my $count = $node->safe_psql('postgres', 'SELECT count(*) FROM t');
	is($count, $c->{rows}, "$c->{name}: all rows loaded");
}

$node->stop;

done_testing();
