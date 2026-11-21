# Copyright (c) 2026, PostgreSQL Global Development Group

# Test completion logging for a slow end-of-recovery step.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;
$primary->backup('backup');

my $perlbin = $^X;
$perlbin =~ s!\\!/!g if $windows_os;

for my $interval ('100ms', '0')
{
	my $standby = PostgreSQL::Test::Cluster->new("standby_$interval");
	$standby->init_from_backup($primary, 'backup', has_streaming => 1);
	$standby->append_conf(
		'postgresql.conf', qq(
log_startup_progress_interval = '$interval'
recovery_end_command = '"$perlbin" -e "sleep 1; print qq(done)" > recovery_end_command.done'
));
	$standby->start;
	$primary->wait_for_replay_catchup($standby);

	my $log_offset = -s $standby->logfile;
	$standby->promote;
	$standby->wait_for_log(qr/database system is ready to accept connections/,
		$log_offset);

	is(slurp_file($standby->data_dir . '/recovery_end_command.done'),
		'done', "recovery_end_command completed with interval $interval");

	my $progress =
	  qr/end of recovery: "recovery_end_command" took \d+\.\d{2} s/;
	if ($interval eq '0')
	{
		unlike(
			slurp_file($standby->logfile, $log_offset),
			qr/end of recovery:/,
			'zero interval suppresses end-of-recovery progress');
	}
	else
	{
		ok( $standby->wait_for_log(
				qr/archive recovery complete.*$progress.*database system is ready to accept connections/s,
				$log_offset),
			'slow recovery_end_command is logged during promotion');
	}

	$standby->stop;
}

done_testing();
