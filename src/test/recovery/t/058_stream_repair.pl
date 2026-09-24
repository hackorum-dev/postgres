# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that streaming replication can replace corrupt WAL that walreceiver
# previously reported as flushed, without moving pg_last_wal_receive_lsn()
# backward on an ordinary walreceiver restart.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# pack() of xl_prev must match on-disk endianness.  'Q' is not available in
# all Perl builds, so split the 64-bit LSN into two 32-bit fields.
my $BIG_ENDIAN = pack('L', 0x12345678) eq pack('N', 0x12345678);

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', qq(
autovacuum = off
wal_keep_size = 1GB
));
$primary->start;

$primary->backup('backup');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->append_conf(
	'postgresql.conf', qq(
# Prefetch would decode the record we later overwrite, so the injected
# prev-link would never be observed.
recovery_prefetch = off
wal_retrieve_retry_interval = '100ms'
));
$standby->start;

$primary->wait_for_catchup($standby);

# ---------------------------------------------------------------------------
# Ordinary walreceiver restart with apply lag must not rewind the shared
# flush pointer.  Pause replay so RecPtr is behind flushedUpto, then change
# primary_conninfo.  Resume so startup notices pendingWalRcvRestart and
# relaunches walreceiver.
# ---------------------------------------------------------------------------
$standby->safe_psql('postgres', 'SELECT pg_wal_replay_pause()');
$standby->poll_query_until(
	'postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "timed out while waiting for recovery to pause";
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$primary->emit_wal(8192);
$primary->wait_for_catchup($standby, 'flush', $primary->lsn('flush'));

my $receive_before = $standby->safe_psql('postgres',
	'SELECT pg_last_wal_receive_lsn()');
my $pid_before = $standby->safe_psql('postgres',
	'SELECT pid FROM pg_stat_wal_receiver');
die "walreceiver not running" unless $pid_before;

my $conninfo = $standby->safe_psql('postgres', 'SHOW primary_conninfo');
$standby->append_conf('postgresql.conf',
	"primary_conninfo = '$conninfo application_name=stream_repair'");
$standby->reload;

# pendingWalRcvRestart is consumed in WaitForWALToBecomeAvailable(), which
# does not run while recovery is paused.  Resume so the restart happens
# while RecPtr is still behind flushedUpto (replay has not caught up yet).
$standby->safe_psql('postgres', 'SELECT pg_wal_replay_resume()');
$standby->poll_query_until(
	'postgres',
	"SELECT EXISTS (SELECT FROM pg_stat_wal_receiver WHERE pid <> $pid_before)"
) or die "timed out waiting for walreceiver restart after primary_conninfo reload";

my $receive_delta = $standby->safe_psql(
	'postgres',
	"SELECT pg_wal_lsn_diff(pg_last_wal_receive_lsn(), '$receive_before')");
ok($receive_delta >= 0,
	'receive LSN does not move backward on walreceiver restart with apply lag');

$primary->wait_for_catchup($standby);

# ---------------------------------------------------------------------------
# Corrupt an unreplayed record after receive has moved into the next segment.
# Startup must re-stream the record instead of looping on the local copy.
# ---------------------------------------------------------------------------
$standby->safe_psql('postgres', 'SELECT pg_wal_replay_pause()');
$standby->poll_query_until(
	'postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "timed out while waiting for recovery to pause";

$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');

# Capture the start of an unreplayed record, then stream well past it so
# walreceiver's flushedUpto is ahead of the later corruption.
my $record_start = int(
	$primary->safe_psql(
		'postgres', "SELECT pg_current_wal_insert_lsn() - '0/0'"));
$primary->emit_wal(1024);
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$primary->emit_wal(8192);
my $target_lsn = $primary->lsn('flush');
$primary->wait_for_catchup($standby, 'flush', $target_lsn);

# Stop walreceiver without restarting the postmaster, preserving its
# flushedUpto high-water mark in shared memory.
my $walreceiver_pid = $standby->safe_psql(
	'postgres', 'SELECT pid FROM pg_stat_wal_receiver');
kill 'TERM', $walreceiver_pid
  or die "could not terminate walreceiver $walreceiver_pid: $!";
$standby->poll_query_until(
	'postgres',
	'SELECT NOT EXISTS (SELECT FROM pg_stat_wal_receiver)')
  or die "timed out while waiting for walreceiver to stop";

# Overwrite the unreplayed record header with a plausible but incorrect
# prev-link.  The primary retains the correct copy.  The postmaster is
# left running so WalRcv shared memory is not reset.
my $wal_segment_size = int(
	$standby->safe_psql(
		'postgres',
		"SELECT setting FROM pg_settings WHERE name = 'wal_segment_size'"));
my $tli = int(
	$standby->safe_psql(
		'postgres', 'SELECT timeline_id FROM pg_control_checkpoint()'));
# XLogRecord: xl_tot_len, xl_xid, xl_prev, xl_info, xl_rmid, pad, xl_crc.
# xl_tot_len=24 and xl_prev=0xdeadbeef fail ValidXLogRecordHeader.
$standby->write_wal(
	$tli, $record_start,
	$wal_segment_size,
	pack(
		'IIIICCBBI',
		24, 0,
		$BIG_ENDIAN ? 0           : 0xdeadbeef,
		$BIG_ENDIAN ? 0xdeadbeef : 0,
		0, 0, 0, 0, 0));

$standby->safe_psql('postgres', 'SELECT pg_wal_replay_resume()');

$standby->poll_query_until(
	'postgres',
	"SELECT pg_last_wal_replay_lsn() >= '$target_lsn'")
  or die "standby did not replace corrupt WAL and catch up";

ok( $standby->log_contains(
		qr/record with incorrect prev-link 0\/DEADBEEF/),
	'standby observed the injected corrupt record');
ok( $standby->log_contains(
		qr/restarting WAL streaming from .* ignoring previously flushed WAL/),
	'standby ignored stale flush pointer after the corrupt record');

$standby->stop;
$primary->stop;

done_testing();
