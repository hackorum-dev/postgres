# Copyright (c) 2026, PostgreSQL Global Development Group

# A physical sender can read WAL from buffers after the corresponding local
# segment has been recycled. The copied pages do not need a local WAL file.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (!defined($ENV{enable_injection_points})
	|| $ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1, extra => ['--wal-segsize=1']);
$primary->append_conf('postgresql.conf', qq(
shared_preload_libraries = 'injection_points'
archive_mode = off
wal_buffers = '32MB'
min_wal_size = '2MB'
max_wal_size = '4MB'
wal_keep_size = 0
checkpoint_timeout = '1h'
wal_sender_timeout = 0
autovacuum = off
));
$primary->start;
$primary->safe_psql('postgres',
	'CREATE EXTENSION injection_points; CREATE TABLE buffers_test (i int)');
$primary->backup('base');

my $receiver = PostgreSQL::Test::Cluster->new('receiver');
$receiver->init_from_backup($primary, 'base', has_streaming => 1);
$receiver->append_conf('postgresql.conf', qq(
restore_command = ''
wal_receiver_timeout = 0
wal_retrieve_retry_interval = '100ms'
));
$receiver->start;
$primary->safe_psql('postgres', 'INSERT INTO buffers_test VALUES (1)');
my $initial_lsn = $primary->lsn('insert');
$primary->wait_for_catchup($receiver, 'replay', $initial_lsn);
my $old_segment = $primary->safe_psql('postgres',
	"SELECT pg_walfile_name('$initial_lsn')");
my $sender_pid = $primary->safe_psql('postgres',
	'SELECT pid FROM pg_stat_replication');

# Prevent the walwriter from preinitializing the buffer pages that the
# sender will need. Pause before XLogBackgroundFlush(), outside its locks.
$primary->safe_psql(
	'postgres',
	q{SELECT injection_points_attach('walwriter-before-background-flush', 'wait')});
$primary->wait_for_event('walwriter', 'walwriter-before-background-flush');

# Keep the existing sender, whose reader already knows the timeline, and
# pause it before reading the next batch of WAL.
$primary->safe_psql('postgres',
	q{SELECT injection_points_attach('walsender-before-wal-read', 'wait')});
$primary->safe_psql('postgres', 'INSERT INTO buffers_test VALUES (2)');
$primary->wait_for_event('walsender', 'walsender-before-wal-read');

# Generate enough WAL for checkpoint recycling to overtake the sender,
# while keeping all the generated data within the 32 MB of WAL buffers.
$primary->safe_psql('postgres', q{
SELECT pg_logical_emit_message(false, 'fill', repeat('x', 1024 * 1024))
FROM generate_series(1, 12);
CHECKPOINT;
});
my $target_lsn = $primary->lsn('insert');
ok(!-e $primary->data_dir . "/pg_wal/$old_segment",
	'checkpoint removed the segment needed by the paused sender')
	  or BAIL_OUT('checkpoint did not remove the required local segment');

$primary->safe_psql('postgres', q{
SELECT injection_points_detach('walsender-before-wal-read');
SELECT injection_points_wakeup('walsender-before-wal-read');
});
my $caught_up = $receiver->poll_query_until('postgres',
	"SELECT pg_last_wal_replay_lsn() >= '$target_lsn'::pg_lsn");
ok($caught_up, 'receiver catches up using retained WAL buffer pages');

SKIP:
{
	skip 'receiver did not catch up', 2 unless $caught_up;
	is($receiver->safe_psql('postgres', 'SELECT i FROM buffers_test ORDER BY i'),
		"1\n2", 'receiver replayed both rows');
	is($primary->safe_psql('postgres', 'SELECT pid FROM pg_stat_replication'),
		$sender_pid, 'the original sender remained connected');
}

$receiver->stop;
$primary->poll_query_until('postgres',
	'SELECT count(*) = 0 FROM pg_stat_replication')
	  or die 'sender did not exit';
my $hits = $primary->safe_psql('postgres', q{
SELECT coalesce(sum(hits), 0) FROM pg_stat_io
WHERE backend_type = 'walsender' AND object = 'wal'
});
cmp_ok($hits, '>', 0, 'walsender read WAL buffer pages');

$primary->safe_psql('postgres', q{
SELECT injection_points_detach('walwriter-before-background-flush');
SELECT injection_points_wakeup('walwriter-before-background-flush');
});
$primary->stop;
done_testing();
