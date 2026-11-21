# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Check that the control file of a standby tracks the standby's own
# wal_log_hints, not the value of its primary, so that pg_rewind refuses a
# standby target that may have written hint bits without WAL.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# allows_streaming sets wal_log_hints = on on the primary.
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1, no_data_checksums => 1);
$node_primary->start;

$node_primary->backup('my_backup');
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, 'my_backup',
	has_streaming => 1);
$node_standby->append_conf('postgresql.conf', 'wal_log_hints = off');
$node_standby->start;

# A second standby that keeps wal_log_hints enabled.
my $node_standby_on = PostgreSQL::Test::Cluster->new('standby_on');
$node_standby_on->init_from_backup($node_primary, 'my_backup',
	has_streaming => 1);
$node_standby_on->start;

# Have the standby replay a parameter change record, which carries the
# primary's wal_log_hints.
$node_primary->append_conf('postgresql.conf', 'track_commit_timestamp = on');
$node_primary->restart;
$node_primary->wait_for_replay_catchup($node_standby);
$node_primary->wait_for_replay_catchup($node_standby_on);
$node_standby->stop;
$node_standby_on->stop;

command_like(
	[ 'pg_controldata', $node_standby->data_dir ],
	qr/wal_log_hints setting:\s+off/,
	'control file of standby has its own wal_log_hints');

my @pg_rewind_cmd = (
	'pg_rewind',
	'--source-server' => $node_primary->connstr('postgres'),
	'--target-pgdata' => $node_standby->data_dir);

command_fails_like(
	\@pg_rewind_cmd,
	qr/target server needs to use either data checksums or "wal_log_hints = on"/,
	'pg_rewind fails with standby target that ran with wal_log_hints = off');

# Enabling wal_log_hints without starting the standby does not help, hint
# bits may already have been written.
$node_standby->append_conf('postgresql.conf', 'wal_log_hints = on');
command_fails_like(
	\@pg_rewind_cmd,
	qr/target server needs to use either data checksums or "wal_log_hints = on"/,
	'pg_rewind fails after enabling wal_log_hints on stopped standby');

# Nor does running with it enabled from now on, the hint bits written
# before are still there.
$node_standby->start;
$node_standby->stop;
command_like(
	[ 'pg_controldata', $node_standby->data_dir ],
	qr/wal_log_hints setting:\s+off/,
	'control file of standby remembers that wal_log_hints was off');
command_fails_like(
	\@pg_rewind_cmd,
	qr/A standby must have run with "wal_log_hints = on" ever since/,
	'pg_rewind fails after restarting standby with wal_log_hints = on');

command_checks_all(
	[
		'pg_rewind',
		'--source-server' => $node_primary->connstr('postgres'),
		'--target-pgdata' => $node_standby_on->data_dir
	],
	0,
	[qr/^$/],
	[qr/no rewind required/],
	'pg_rewind runs with standby target that always ran with wal_log_hints = on');

# At promotion, the standby logs its own parameters even when they match
# pg_control, which no longer holds the primary's wal_log_hints.
$node_standby->append_conf(
	'postgresql.conf', qq(
wal_log_hints = off
track_commit_timestamp = on
));
$node_standby->start;
my $switch_lsn =
  $node_standby->safe_psql('postgres', 'SELECT pg_last_wal_replay_lsn()');
$node_standby->promote;
my $end_lsn =
  $node_standby->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
$node_standby->stop;
command_like(
	[
		'pg_waldump',
		'--path' => $node_standby->data_dir . '/pg_wal',
		'--timeline' => 2,
		'--start' => $switch_lsn,
		'--end' => $end_lsn,
	],
	qr/PARAMETER_CHANGE .*wal_log_hints=off/,
	'promoted standby logs its own wal_log_hints');

done_testing();
