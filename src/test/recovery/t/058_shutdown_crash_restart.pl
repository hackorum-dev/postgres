# Copyright (c) 2026, PostgreSQL Global Development Group

# Test fast shutdown during crash restart, before WAL redo has started.

use strict;
use warnings FATAL => 'all';
use FindBin;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init(allows_streaming => 1);

# Make the restarted startup process wait in restore_command until shutdown.
my $perlbin = $^X;
$perlbin =~ s!\\!/!g if $windows_os;
my $logfile = $node->logfile;
$logfile =~ s!\\!/!g if $windows_os;
my $timeout = $PostgreSQL::Test::Utils::timeout_default;
my $restore_timeout = 4 * $timeout;
$node->append_conf(
	'postgresql.conf', qq{
restart_after_crash = on
log_min_messages = debug2
restore_command = '"$perlbin" "$FindBin::RealBin/wait_for_shutdown" "$logfile" $restore_timeout'
});
$node->start;

$node->poll_query_until(
	'postgres',
	q{SELECT count(*) = 1 FROM pg_stat_activity
	  WHERE backend_type = 'background writer'}
) or die 'background writer did not start';
my $pid = $node->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE backend_type = 'background writer'"
);
$node->set_standby_mode;
my $log_offset = -s $node->logfile;
system_or_bail('pg_ctl', 'kill', 'QUIT', $pid);
$node->wait_for_log(qr/restore_command waiting for shutdown/, $log_offset);
# Wait until the new checkpointer has installed its SIGTERM handler.
$node->wait_for_log(
	qr/checkpointer updated shared memory configuration values/, $log_offset);

ok( $node->stop('fast', fail_ok => 1, timeout => $timeout),
	'fast shutdown completes during crash restart');
# pg_ctl can report success after a helper timeout made startup fail.
unlike(
	slurp_file($node->logfile, $log_offset),
	qr/timed out waiting for shutdown request/,
	'restore_command did not time out');

done_testing();
