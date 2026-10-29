# Copyright (c) 2026, PostgreSQL Global Development Group

# Inherited ownership works across sessions and recovery without SET ROLE.
# Failed authorization and database checks must leave the entry unlocked.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 2');
$node->start;
$node->safe_psql(
	'postgres', qq(
	CREATE ROLE prepare_owner LOGIN;
	CREATE ROLE prepare_member LOGIN;
	CREATE ROLE prepare_other LOGIN;
	GRANT prepare_owner TO prepare_member WITH INHERIT TRUE, SET FALSE;
	CREATE TABLE prepared_data (a int);
	ALTER TABLE prepared_data OWNER TO prepare_owner;
	CREATE DATABASE otherdb;
));

my ($ret, $stdout, $stderr) = $node->psql(
	'postgres', 'SET ROLE prepare_owner',
	extra_params => [ '-U', 'prepare_member' ]);
is($ret, 3, 'member cannot SET ROLE to the preparing role');
like($stderr, qr/permission denied to set role/,
	'SET FALSE prevents switching to the preparing role');

for my $mode ('restart', 'crash recovery')
{
	$node->safe_psql(
		'postgres', qq(
		BEGIN;
		INSERT INTO prepared_data VALUES (1);
		PREPARE TRANSACTION 'inherit_commit';
		BEGIN;
		INSERT INTO prepared_data VALUES (2);
		PREPARE TRANSACTION 'inherit_rollback';
	),
		extra_params => [ '-U', 'prepare_owner' ]);

	if ($mode eq 'restart')
	{
		$node->safe_psql('postgres', 'CHECKPOINT');
		$node->restart;
	}
	else
	{
		$node->stop('immediate');
		$node->start;
	}

	for my $command ('COMMIT', 'ROLLBACK')
	{
		my $gid = 'inherit_' . lc($command);
		($ret, $stdout, $stderr) = $node->psql(
			'postgres', "$command PREPARED '$gid'",
			extra_params => [ '-U', 'prepare_other' ]);
		is($ret, 3, "$mode: unrelated role cannot $command");
		like($stderr, qr/permission denied to finish prepared transaction/,
			"$mode: $command checks inherited ownership");

		($ret, $stdout, $stderr) = $node->psql(
			'otherdb', "$command PREPARED '$gid'",
			extra_params => [ '-U', 'prepare_member' ]);
		is($ret, 3, "$mode: member cannot $command in another database");
		like($stderr, qr/prepared transaction belongs to another database/,
			"$mode: $command keeps the database restriction");

		($ret, $stdout, $stderr) = $node->psql(
			'postgres', "$command PREPARED '$gid'",
			extra_params => [ '-U', 'prepare_member' ]);
		is($ret, 0,
			"$mode: member can $command after the failed attempts");
	}

	is($node->safe_psql('postgres', 'SELECT count(*) FROM pg_prepared_xacts'),
		'0', "$mode: both transactions resolved");
}

is($node->safe_psql('postgres', 'SELECT a FROM prepared_data ORDER BY a'),
	"1\n1", 'only committed transactions are visible');
$node->stop;
done_testing();
