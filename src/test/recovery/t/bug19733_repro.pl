use strict;
use warnings FATAL => 'all';

use IPC::Run;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Injection points not supported by this build'
  unless ($ENV{enable_injection_points} // '') eq 'yes';

my $node = PostgreSQL::Test::Cluster->new('bug19733');
$node->init(allows_streaming => 'logical');
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', q(
	CREATE EXTENSION injection_points;
	CREATE TABLE bug19733_txn
	(
		id integer PRIMARY KEY,
		version bigint NOT NULL
	);
	INSERT INTO bug19733_txn VALUES (1, 0);
	SELECT pg_create_logical_replication_slot(
		'bug19733_slot', 'test_decoding');
));

my $output_file = $node->basedir . '/logical-output.log';
my ($receiver_stdout, $receiver_stderr);
my $receiver = IPC::Run::start(
	[
		'pg_recvlogical',
		'--dbname' => $node->connstr('postgres'),
		'--slot' => 'bug19733_slot',
		'--start',
		'--file' => $output_file,
		'--fsync-interval' => '1',
	],
	'>' => \$receiver_stdout,
	'2>' => \$receiver_stderr);

my $writer = $node->background_psql('postgres');
my $writer_pid = $writer->query_safe('SELECT pg_backend_pid()');

$writer->query_safe(q(
	SELECT injection_points_set_local();
	SELECT injection_points_attach('commit-before-clog-update', 'wait');
));

$writer->query_until(
	qr/writer-started/,
	q(
		\echo writer-started
		BEGIN;
		UPDATE bug19733_txn
		   SET version = version + 1
		 WHERE id = 1;
		SELECT pg_logical_emit_message(
			true, 'bug19733', '1:1');
		COMMIT;
	));

$node->poll_query_until(
	'postgres',
	"SELECT wait_event = 'commit-before-clog-update' "
	  . "FROM pg_stat_activity WHERE pid = $writer_pid")
  or die 'writer did not reach commit injection point';

wait_for_file(
	$output_file,
	qr/message: transactional: 1 prefix: bug19733, .*content:1:1/);

is(
	$node->safe_psql(
		'postgres',
		'SELECT version FROM bug19733_txn WHERE id = 1'),
	'0',
	'logical message is received while a new snapshot still sees old row');

$node->safe_psql(
	'postgres',
	"SELECT injection_points_wakeup('commit-before-clog-update')");
$writer->quit;

is(
	$node->safe_psql(
		'postgres',
		'SELECT version FROM bug19733_txn WHERE id = 1'),
	'1',
	'row becomes visible after commit leaves the injection point');

$receiver->signal('TERM');
$receiver->finish();
$node->stop;

done_testing();
