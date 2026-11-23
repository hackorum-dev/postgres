# Copyright (c) 2026, PostgreSQL Global Development Group

# The standby restarts on the old primary's timeline after an interrupted
# promotion. pg_rewind then misses the divergence between the two nodes.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$primary->safe_psql('postgres', 'CREATE TABLE branch (name text)');
$primary->backup('backup');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->start;
$primary->wait_for_replay_catchup($standby);
$primary->stop;

# Interrupt promotion after startup removes standby.signal and writes the
# child timeline's history file. The end-of-recovery record is still
# unwritten and pg_control still points to the parent timeline.
my $point = 'promotion-after-timeline-history';
$standby->safe_psql('postgres',
	"SELECT injection_points_attach('$point', 'wait')");
$standby->safe_psql('postgres', 'SELECT pg_promote(false)');
$standby->wait_for_event('startup', $point);
$standby->stop('immediate');
$standby->start;

# Startup came up as a primary on the parent timeline, so pg_ctl rejects
# the second promotion request.
command_ok(
	[ 'pg_ctl', '--pgdata' => $standby->data_dir, 'promote' ],
	'can retry the interrupted promotion after restart');

# Commit a distinct row on each node so the target has a change to discard
# and the source has a change the target must recover.
$standby->safe_psql('postgres', q{INSERT INTO branch VALUES ('promoted')});
$standby->stop;
$primary->start;
$primary->safe_psql('postgres', q{INSERT INTO branch VALUES ('old primary')});
$primary->stop;

# Both nodes report the same timeline ID. pg_rewind reports "no rewind
# required" and exits successfully despite the divergent rows.
is( system_log(
		'pg_rewind',
		'--source-pgdata' => $standby->data_dir,
		'--target-pgdata' => $primary->data_dir),
	0, 'pg_rewind exits successfully');

$primary->start(options => '-p ' . $primary->port);
# pg_rewind skipped the divergent WAL, so the target still reads
# 'old primary' where it should read 'promoted'.
is($primary->safe_psql('postgres', 'SELECT name FROM branch ORDER BY name'),
	'promoted', 'rewound target contains only the promoted node\'s row');

done_testing();
