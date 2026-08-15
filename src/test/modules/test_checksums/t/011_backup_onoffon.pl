
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test a base backup which is running while data checksums are disabled and
# then re-enabled.  Hint bit updates made while checksums were off reach disk
# without a checksum update and without moving the page LSN, so once the
# re-enabling completes the backup would resume verification and misjudge
# those pages until the rewritten versions are flushed.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('onoffon_node');
$node->init(allows_streaming => 1);
# The pages rewritten while re-enabling must stay dirty in shared buffers
# until the final checkpoint, otherwise they reach disk with checksums on
# their own and nothing is left to misjudge.  Autovacuum is disabled so that
# nothing sets hint bits before checksums are turned off, and wal_log_hints
# (implied by allows_streaming) must be off so that setting them does not
# move the page LSNs past the backup start.
$node->append_conf('postgresql.conf', 'shared_buffers = 128MB');
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->append_conf('postgresql.conf', 'wal_log_hints = off');
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

# Enough data to keep the throttled backup running while checksums are being
# turned off and back on.  The table is not read here, leaving the hint bits
# unset until checksums are off.
$node->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,600000) AS a;");
test_checksum_state($node, 'on');

my $backupdir = $node->backup_dir . '/onoffon';
my ($out, $err) = ('', '');
my $backup = IPC::Run::start(
	[
		'pg_basebackup', '-D', $backupdir,
		'--wal-method=none', '--no-sync',
		'--checkpoint=fast', '--max-rate=4096',
		'-d', $node->connstr('postgres')
	],
	'>', \$out, '2>', \$err,
	IPC::Run::timeout(180));

$node->poll_query_until('postgres',
	    "SELECT count(*) > 0 FROM pg_catalog.pg_stat_activity "
	  . "WHERE backend_type = 'walsender' AND state = 'active';");

disable_data_checksums($node, wait => 1);

# With checksums off, the scan sets hint bits without WAL logging them, and
# the checkpoint flushes the modified pages without updating their checksums.
# The on-disk pages now carry stale checksums and LSNs older than the backup
# start.
$node->safe_psql('postgres', "SELECT count(*) FROM t;");
$node->safe_psql('postgres', "CHECKPOINT;");

# Hold the re-enabling after the state changed to "on" but before the
# checkpoint which flushes the rewritten pages.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('datachecksums-on-before-checkpoint','wait');"
);

enable_data_checksums($node);
$node->wait_for_event('datachecksums launcher',
	'datachecksums-on-before-checkpoint');

# The backup must still be sending files, otherwise it never sees the state
# changes and the test is pointless.
my $running = $node->safe_psql('postgres',
	    "SELECT count(*) FROM pg_catalog.pg_stat_activity "
	  . "WHERE backend_type = 'walsender';");
is($running, '1', 'backup still running when checksums were re-enabled');

ok($backup->finish, 'backup straddling disable and re-enable succeeds')
  or diag("stderr: $err");

$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('datachecksums-on-before-checkpoint');");
$node->safe_psql('postgres',
	"SELECT injection_points_detach('datachecksums-on-before-checkpoint');");

wait_for_checksum_state($node, 'on');
$node->poll_query_until('postgres',
	    "SELECT count(*) = 0 FROM pg_catalog.pg_stat_activity "
	  . "WHERE backend_type = 'datachecksums launcher';");

my $result = $node->safe_psql('postgres',
	"SELECT coalesce(sum(checksum_failures), 0) FROM pg_catalog.pg_stat_database;"
);
is($result, '0', 'no spurious checksum failures reported');

# A backup started once re-enabling has completed must verify, and pass
$node->command_ok(
	[
		'pg_basebackup', '-D', $node->backup_dir . '/after',
		'--wal-method=none', '--no-sync', '--checkpoint=fast'
	],
	'backup after re-enable completion succeeds');

$node->stop;
done_testing();
