# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_rewind across offline checksum enables on both nodes.  The last common
# checkpoint still carries "off"; the rewound server must not adopt that
# over the "on" both sides were moved to with pg_checksums, since no record
# in the replayed WAL could ever restore it.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

# wal_log_hints keeps the pair eligible for pg_rewind without checksums.
my $node_a = PostgreSQL::Test::Cluster->new('node_a');
$node_a->init(allows_streaming => 1, no_data_checksums => 1);
$node_a->append_conf(
	'postgresql.conf', qq[
autovacuum = off
wal_keep_size = '1GB'
wal_log_hints = on
]);
$node_a->start;
$node_a->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,10000) AS a;");

$node_a->backup('backup');
my $node_b = PostgreSQL::Test::Cluster->new('node_b');
$node_b->init_from_backup($node_a, 'backup', has_streaming => 1);
$node_b->start;
$node_a->wait_for_catchup($node_b);

# Failover to B, and divergence on A.
$node_b->promote;
$node_b->safe_psql('postgres', "INSERT INTO t VALUES (0);");
$node_a->safe_psql('postgres', "INSERT INTO t VALUES (-1);");
$node_a->stop('fast');

# The lockstep procedure across the divergence: both nodes get their
# checksums enabled offline.
$node_a->checksum_enable_offline;
$node_b->stop;
$node_b->checksum_enable_offline;
$node_b->start;
test_checksum_state($node_b, 'on');

# The states match, so the rewind proceeds without complaint.
command_ok(
	[
		'pg_rewind',
		'--target-pgdata' => $node_a->data_dir,
		'--source-server' => $node_b->connstr('postgres'),
	],
	'pg_rewind with checksums enabled offline on both nodes');

$node_a->append_conf('postgresql.conf', 'port = ' . $node_a->port);
$node_a->enable_streaming($node_b);
$node_a->set_standby_mode;

my $log_offset = -s $node_a->logfile;
$node_a->start;
$node_b->wait_for_catchup($node_a);

is($node_a->safe_psql('postgres', "SELECT count(*) FROM t;"),
	'10001', 'rewound server readable as a standby');

# The offline enable survives: replay from the common checkpoint must not
# resurrect the pre-divergence "off".
test_checksum_state($node_a, 'on');

my $log = PostgreSQL::Test::Utils::slurp_file($node_a->logfile, $log_offset);
unlike(
	$log,
	qr/does not match the state/,
	'no divergence reported between the rewound server and its source');

$node_a->stop;
$node_b->stop;

done_testing();
