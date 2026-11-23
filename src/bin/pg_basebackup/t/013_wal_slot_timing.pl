# Copyright (c) 2026, PostgreSQL Global Development Group

# Check that pg_basebackup's replication slot reserves WAL from before the
# backup's start position.  If the slot were created only after BASE_BACKUP
# had returned the start position, a checkpoint running in between could
# remove the WAL the backup needs, and streaming it would fail.
#
# We hold the slot creation with an injection point, force WAL past several
# segments and checkpoint, then let the slot creation continue.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $tempdir = PostgreSQL::Test::Utils::tempdir;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
wal_keep_size = 0
checkpoint_timeout = 1h
});
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres',
	"SELECT injection_points_attach('replication-slot-create-begin', 'wait')"
);

my $backupdir = "$tempdir/backup";
my ($stdout, $stderr);
my $h = IPC::Run::start(
	[
		'pg_basebackup', '--no-sync',
		'--checkpoint' => 'fast',
		'--pgdata' => $backupdir,
		'--wal-method' => 'stream',
		'--dbname' => $node->connstr('postgres'),
	],
	'>' => \$stdout,
	'2>' => \$stderr);

# Wait until the walsender is about to create pg_basebackup's slot.
$node->wait_for_event('walsender', 'replication-slot-create-begin');

# Move WAL forward by several segments and checkpoint, so that every segment
# not protected by a slot or wal_keep_size is removed or recycled.
$node->advance_wal(3);
$node->safe_psql('postgres', 'CHECKPOINT');

$node->safe_psql('postgres',
	"SELECT injection_points_detach('replication-slot-create-begin')");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('replication-slot-create-begin')");

my $ok = $h->finish;
ok($ok, 'pg_basebackup succeeds with WAL activity during slot creation')
  or diag("pg_basebackup stderr:\n$stderr");
unlike(
	$stderr,
	qr/has already been removed/,
	'no WAL needed by the backup was removed');

# pg_verifybackup also parses the WAL the backup needs, so this fails if
# any of it is missing.
$node->command_ok([ 'pg_verifybackup', $backupdir ],
	'backup with streamed WAL verifies');

# The slot is idle until the receiver starts after BASE_BACKUP, so the
# backup's checkpoint can invalidate it under idle_replication_slot_timeout.
# A temporary slot stays owned by its session, so the checkpoint terminates
# that session, and pg_basebackup must recreate the slot.  With
# slot-timeout-inval attached, every checkpoint invalidates every idle slot,
# however briefly it has been idle.
$node->append_conf('postgresql.conf', 'idle_replication_slot_timeout = 1h');
$node->reload;
$node->safe_psql('postgres',
	"SELECT injection_points_attach('slot-timeout-inval', 'error')");

my $idle_offset = -s $node->logfile;
$node->command_checks_all(
	[
		'pg_basebackup', '--no-sync',
		'--checkpoint' => 'fast',
		'--pgdata' => "$tempdir/backup_idle",
		'--wal-method' => 'stream',
		'--dbname' => $node->connstr('postgres'),
	],
	0,
	[],
	[
		qr/replication slot "pg_basebackup_\d+" was invalidated during the base backup checkpoint, recreating it/
	],
	'backup with a temporary slot invalidated by idle_replication_slot_timeout'
);

$node->safe_psql('postgres',
	"SELECT injection_points_detach('slot-timeout-inval')");

# Make sure it was the idle timeout that invalidated the slot.
ok( $node->log_contains(
		qr/terminating process \d+ to release replication slot "pg_basebackup_\d+"\n.*DETAIL:  The slot's idle time/,
		$idle_offset),
	"backup's checkpoint invalidated the idle temporary slot");

$node->command_ok([ 'pg_verifybackup', "$tempdir/backup_idle" ],
	'backup with a slot recreated after the idle timeout verifies');

# The slot is created before the backup's checkpoint, so it starts at the
# previous one.  If more WAL than max_slot_wal_keep_size was written since
# then, the backup's checkpoint invalidates the slot.  pg_basebackup must then
# recreate it and still get all the WAL the backup needs.
my $knode = PostgreSQL::Test::Cluster->new('keep');
$knode->init(allows_streaming => 1, extra => ['--wal-segsize=1']);
$knode->append_conf(
	'postgresql.conf', q{
wal_keep_size = 0
max_slot_wal_keep_size = 2MB
checkpoint_timeout = 1h
});
$knode->start;

foreach my $case (
	[ 'temporary slot', 'backup_keep_temp', [] ],
	[
		'--create-slot', 'backup_keep_created',
		[ '--create-slot', '--slot' => 'keep_slot' ]
	])
{
	my ($desc, $dirname, $slot_opts) = @$case;
	my $dir = "$tempdir/$dirname";

	$knode->safe_psql('postgres', 'CHECKPOINT');
	$knode->advance_wal(5);

	my $log_offset = -s $knode->logfile;
	$knode->command_checks_all(
		[
			'pg_basebackup', '--no-sync',
			'--checkpoint' => 'fast',
			'--pgdata' => $dir,
			'--wal-method' => 'stream',
			@$slot_opts,
			'--dbname' => $knode->connstr('postgres'),
		],
		0,
		[],
		[qr/replication slot ".*" was invalidated during the base backup checkpoint, recreating it/],
		"backup with a $desc invalidated by max_slot_wal_keep_size");

	# Make sure the backup's checkpoint really invalidated the slot.  A
	# temporary slot is owned by its session, so that session is terminated.
	ok( $knode->log_contains(
			qr/(?:invalidating obsolete|terminating process \d+ to release) replication slot "(?:pg_basebackup_\d+|keep_slot)"/,
			$log_offset),
		"backup's checkpoint invalidated the $desc");

	$knode->command_ok([ 'pg_verifybackup', $dir ],
		"backup with a recreated $desc verifies");
}

is( $knode->safe_psql(
		'postgres',
		q{SELECT restart_lsn IS NOT NULL AND invalidation_reason IS NULL
			FROM pg_replication_slots WHERE slot_name = 'keep_slot'}),
	't',
	'slot recreated for --create-slot is valid after the backup');
$knode->safe_psql('postgres', "SELECT pg_drop_replication_slot('keep_slot')");

done_testing();
