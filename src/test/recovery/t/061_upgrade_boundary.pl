# Copyright (c) 2026, PostgreSQL Global Development Group

# Test marking a shutdown checkpoint as an upgrade boundary: the
# deliberate end of this major version's WAL, pending an in-place upgrade.
# The marking is visible in the control file and in the WAL itself, and a
# cluster simply restarted on the same binary cancels it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Fetch one field from the control file
sub control_field
{
	my ($node, $field) = @_;

	my ($stdout, $stderr) =
	  run_command([ 'pg_controldata', $node->data_dir ]);
	die "$field not found in control file\n"
	  unless $stdout =~ /^\Q$field\E:\s*(.*)$/m;

	return $1;
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init;
$primary->start;

# Request state round trip on the primary
is( $primary->safe_psql('postgres', 'SELECT pg_upgrade_boundary_requested()'),
	'f',
	'initially not active');
$primary->safe_psql('postgres', 'SELECT pg_request_upgrade_boundary()');
is( $primary->safe_psql('postgres', 'SELECT pg_upgrade_boundary_requested()'),
	't',
	'active after request');
$primary->safe_psql('postgres', 'SELECT pg_cancel_upgrade_boundary()');
is( $primary->safe_psql('postgres', 'SELECT pg_upgrade_boundary_requested()'),
	'f',
	'cancelled again');
$primary->safe_psql('postgres', 'SELECT pg_cancel_upgrade_boundary()');
is( $primary->safe_psql('postgres', 'SELECT pg_upgrade_boundary_requested()'),
	'f',
	'cancelling with nothing requested is a no-op');

# An online checkpoint neither consumes nor acts on a pending request;
# only a shutdown checkpoint does.
$primary->safe_psql('postgres', 'SELECT pg_request_upgrade_boundary()');
$primary->safe_psql('postgres', 'CHECKPOINT');
is(control_field($primary, "Latest checkpoint's upgrade flag"),
	'none', 'online checkpoint is not marked as a boundary');
is( $primary->safe_psql('postgres', 'SELECT pg_upgrade_boundary_requested()'),
	't',
	'request stays active across an online checkpoint');
$primary->safe_psql('postgres', 'SELECT pg_cancel_upgrade_boundary()');

# Shutting down with a request pending marks the shutdown checkpoint, and
# the control file keeps the marking, so the next binary started on this
# data directory can see it.
$primary->safe_psql('postgres', 'SELECT pg_request_upgrade_boundary()');
$primary->stop('fast');
is(control_field($primary, "Latest checkpoint's upgrade flag"),
	'boundary', 'shutdown checkpoint is marked as a boundary');

# The flag is in the WAL itself, not just the control file.
command_like(
	[
		'pg_waldump', '-p', $primary->data_dir . '/pg_wal',
		'-s', control_field($primary, 'Latest checkpoint location'),
		'-n', '1'
	],
	qr/CHECKPOINT_SHUTDOWN .*upgrade flag boundary/,
	'pg_waldump shows the flag on the boundary checkpoint record');

# pg_resetwal discards the WAL after the boundary checkpoint and writes a
# new one in its place, so the marking must not carry over to it.
command_ok([ 'pg_resetwal', $primary->data_dir ], 'pg_resetwal succeeds');
is(control_field($primary, "Latest checkpoint's upgrade flag"),
	'none', 'pg_resetwal clears the boundary marking');
command_like(
	[
		'pg_waldump', '-p', $primary->data_dir . '/pg_wal',
		'-s', control_field($primary, 'Latest checkpoint location'),
		'-n', '1'
	],
	qr/CHECKPOINT_SHUTDOWN (?!.*upgrade flag)/,
	'checkpoint written by pg_resetwal is not marked');

# A boundary can be requested again on the reset cluster.
$primary->start;
$primary->safe_psql('postgres', 'SELECT pg_request_upgrade_boundary()');
$primary->stop('fast');
is(control_field($primary, "Latest checkpoint's upgrade flag"),
	'boundary', 'boundary re-established after pg_resetwal');

# Abandon the upgrade: starting the primary on the same binary cancels the
# boundary as a side effect of writing new WAL on this timeline.
$primary->start;
ok( $primary->log_contains(
		'database system was shut down at an upgrade boundary'),
	'primary logged the cancelled boundary');
is( $primary->safe_psql('postgres', 'SELECT pg_upgrade_boundary_requested()'),
	'f',
	'restarting does not re-request');
$primary->safe_psql('postgres', 'CHECKPOINT');
is(control_field($primary, "Latest checkpoint's upgrade flag"),
	'none', 'cancelled boundary is gone from the primary control file');

done_testing();
