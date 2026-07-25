
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test a base backup which is running when enabling data checksums completes.
# The transition to "on" happens before the checkpoint which flushes the
# rewritten pages, so such a backup reads on-disk pages which legitimately
# lack checksums and carry LSNs older than the backup start.

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

my $node = PostgreSQL::Test::Cluster->new('straddle_node');
$node->init(no_data_checksums => 1, allows_streaming => 1);
# The pages rewritten while enabling must stay dirty in shared buffers until
# the final checkpoint, otherwise they reach disk with checksums on their own
# and nothing is left to misjudge.
$node->append_conf('postgresql.conf', 'shared_buffers = 128MB');
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

# Enough data to keep the throttled backup running while checksums are being
# enabled.  The scan pulls the table into shared buffers so that enabling
# doesn't read it through a ring buffer, which would write the pages back out.
$node->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,600000) AS a;");
$node->safe_psql('postgres', "SELECT count(*) FROM t;");
test_checksum_state($node, 'off');

# Hold the transition after the state changed to "on" but before the
# checkpoint which flushes the rewritten pages.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('datachecksums-on-before-checkpoint','wait');"
);

my $backupdir = $node->backup_dir . '/straddle';
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

enable_data_checksums($node);
$node->wait_for_event('datachecksums launcher',
	'datachecksums-on-before-checkpoint');

# The backup must still be sending files, otherwise it never sees the state
# change and the test is pointless.
my $running = $node->safe_psql('postgres',
	    "SELECT count(*) FROM pg_catalog.pg_stat_activity "
	  . "WHERE backend_type = 'walsender';");
is($running, '1', 'backup still running when checksums were enabled');

ok($backup->finish, 'backup straddling enable completion succeeds')
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

# A backup started once enabling has completed must verify, and pass
$node->command_ok(
	[
		'pg_basebackup', '-D', $node->backup_dir . '/after',
		'--wal-method=none', '--no-sync', '--checkpoint=fast'
	],
	'backup after enable completion succeeds');

$node->stop;
done_testing();
