# Copyright (c) 2026, PostgreSQL Global Development Group

# A checkpoint that runs between the XLOG2_CHECKSUMS("on") record and the
# control file write at the end of SetDataChecksumsOn() must not make a crash
# throw the completed transition away.
#
# SetDataChecksumsOn() writes the record, flips shared memory to "on", emits
# the barrier and only then requests the checkpoint that flushes the rewritten
# pages; the control file is written after that checkpoint returns.  A crash in
# that window is harmless only as long as recovery still starts before the
# record.  Any checkpoint completing in the window moves the redo point past
# the record, so recovery would never see it, would come up with the control
# file's "inprogress-on" and StartupXLOG() would demote that to "off", even
# though every page on disk carries a checksum by then.
#
# CreateCheckPoint() therefore persists the state the checkpoint ran under, the
# same way CreateRestartPoint() does.  The window is naturally reachable:
# checkpoint_timeout, max_wal_size, an explicit CHECKPOINT, pg_basebackup or
# pg_backup_start can all fire there.  Here it is made deterministic by holding
# the launcher at the datachecksums-on-before-checkpoint injection point and
# checkpointing from another session.

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

my $node = PostgreSQL::Test::Cluster->new('concurrent_checkpoint');
$node->init(no_data_checksums => 1);
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 10GB
));
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

test_checksum_state($node, 'off');

$node->safe_psql('postgres',
	'CREATE TABLE t AS SELECT generate_series(1,10000) AS a;');
my $relpath =
  $node->safe_psql('postgres', "SELECT pg_relation_filepath('t'::regclass);");

# Hold the launcher after the record, the shared memory flip and the barrier,
# but before the checkpoint SetDataChecksumsOn() requests itself.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('datachecksums-on-before-checkpoint','wait');"
);

enable_data_checksums($node);
$node->wait_for_event('datachecksums launcher',
	'datachecksums-on-before-checkpoint');

# Every backend already sees "on" and writes checksums.
test_checksum_state($node, 'on');

# ... while the control file still says "inprogress-on".
my ($ctl) = run_command([ 'pg_controldata', $node->data_dir ]);
my ($before) = $ctl =~ /Data page checksum version:\s+(\d+)/;
is($before, '3', 'control file says "inprogress-on" inside the window');

# A concurrent checkpoint.  It flushes the rewritten pages and moves the redo
# point past the XLOG2_CHECKSUMS("on") record, so it has to record the "on"
# state in the control file as well.
$node->safe_psql('postgres', 'CHECKPOINT;');

($ctl) = run_command([ 'pg_controldata', $node->data_dir ]);
my ($after) = $ctl =~ /Data page checksum version:\s+(\d+)/;
is($after, '1',
	'the concurrent checkpoint records "on" in the control file');

$node->stop('immediate');

# The checkpoint flushed the rewritten pages, so they carry a checksum on
# disk: the transition really did complete.
my $page;
open(my $fh, '<', $node->data_dir . '/' . $relpath) or die $!;
binmode $fh;
read($fh, $page, 8192);
close($fh);
my ($pd_checksum) = unpack('x8 v', $page);
note("on-disk pd_checksum of t block 0: $pd_checksum");
isnt($pd_checksum, 0, 'block 0 of t on disk carries a checksum');

my $log_offset = -s $node->logfile;
$node->start;

# The transition is complete on disk, so the cluster has to come back "on".
test_checksum_state($node, 'on');

my $log = PostgreSQL::Test::Utils::slurp_file($node->logfile, $log_offset);
unlike(
	$log,
	qr/enabling data checksums was interrupted/,
	'the completed transition is not reported as interrupted');

$node->stop;

done_testing();
