# Copyright (c) 2026, PostgreSQL Global Development Group

# An offline pg_checksums change on a standby must survive the re-replay of
# an older XLOG2_CHECKSUMS record.  A standby that replayed the final "on"
# record and stopped cleanly before any restartpoint moved past it resumes
# replay below the record on the next startup; without the watermark in the
# control file, re-applying it would silently revert an offline disable made
# while the standby was down.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(no_data_checksums => 1, allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', qq(
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 10GB
wal_keep_size = 1GB
));
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->append_conf('postgresql.conf', 'checkpoint_timeout = 1h');
$standby->start;
$primary->wait_for_catchup($standby);

# A checkpoint record for the standby's shutdown restartpoint to build on,
# with a redo point below the transition records written next.
$primary->safe_psql('postgres', 'CHECKPOINT;');

# Hold the launcher after the XLOG2_CHECKSUMS("on") record and its barrier,
# but before the checkpoint that would move the redo horizon past it.
$primary->safe_psql('postgres',
	"SELECT injection_points_attach('datachecksums-on-before-checkpoint','wait');"
);

enable_data_checksums($primary);
$primary->wait_for_event('datachecksums launcher',
	'datachecksums-on-before-checkpoint');

# The standby replays the "on" record; its restartpoint horizon stays below.
$primary->wait_for_catchup($standby);
wait_for_checksum_state($standby, 'on');

$standby->stop('fast');

# The clean shutdown caught the control file up to "on" while the resume
# point stays below the record.
my ($ctl) = run_command([ 'pg_controldata', $standby->data_dir ]);
my ($version) = $ctl =~ /Data page checksum version:\s+(\d+)/;
is($version, '1', 'standby control file says "on" after the clean stop');

# Disable checksums offline while the standby is down.
$standby->checksum_disable_offline;

# Let the enable finish on the primary.
$primary->safe_psql('postgres',
	"SELECT injection_points_wakeup('datachecksums-on-before-checkpoint');");
$primary->safe_psql('postgres',
	"SELECT injection_points_detach('datachecksums-on-before-checkpoint');");
wait_for_checksum_state($primary, 'on');

# The restart re-reads the WAL below the stop position, including the "on"
# record, which the watermark now marks as already applied.
my $log_offset = -s $standby->logfile;
$standby->start;
$primary->wait_for_catchup($standby);

test_checksum_state($standby, 'off');

# The nodes legitimately diverged, which replayed checkpoints report.
my $log = PostgreSQL::Test::Utils::slurp_file($standby->logfile, $log_offset);
like(
	$log,
	qr/data checksum state "off" of this node does not match the state "on" in the replayed WAL/,
	'the standby reports the divergence from the primary');

$standby->stop;
$primary->stop;

done_testing();
