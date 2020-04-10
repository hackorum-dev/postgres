# Test master/standby switchover scenario where the track_commit_timestamp
# GUC is originally off, then enabled on the new primary after a switchover.
use strict;
use warnings;

use TestLib;
use Test::More tests => 3;
use PostgresNode;

my $bkplabel = 'backup';
my $master   = get_new_node('master');
$master->init(allows_streaming => 1);
$master->append_conf(
	'postgresql.conf', qq{
	track_commit_timestamp = on
	max_wal_senders = 5
	max_worker_processes = 8
	});
$master->start;
$master->backup($bkplabel);

my $standby = get_new_node('standby');
$standby->init_from_backup($master, $bkplabel, has_streaming => 1);
$standby->start;

for my $i (1 .. 10)
{
	$master->safe_psql('postgres', "create table t$i()");
}
my $master_lsn =
  $master->safe_psql('postgres', 'select pg_current_wal_lsn()');
$standby->poll_query_until('postgres',
	qq{SELECT '$master_lsn'::pg_lsn <= pg_last_wal_replay_lsn()})
  or die "standby never caught up";

$standby->safe_psql('postgres', 'checkpoint');
$standby->restart;

# Begin the switchover, and first stop cleanly the primary.
$master->stop;

# Promote the standby with larger max_worker_processes, and enable
# track_commit_timestamp while on it. Generate a XLOG_PARAMETER_CHANGE
# record with those values.
$standby->promote;
$standby->append_conf('postgresql.conf', 'track_commit_timestamp = off');
$standby->append_conf('postgresql.conf', 'max_worker_processes = 10');
$standby->restart;

# Enable streaming on the previous primary, and start it. The node will
# shut down once it finds the record XLOG_PARAMETER_CHANGE generated
# previously.
$master->enable_streaming($standby);
command_fails(
    [ 'pg_ctl', '-D', $master->data_dir, '-l', $master->logfile, 'start' ],
    'start fails because of max_worker_processes lower on standby at recovery');

# Consume some transactions on the standby without the primary knowing
# about it yet, generating some commit timestamps.
for my $i (11 .. 20)
{
	$standby->safe_psql('postgres', "create table t$i()");
}

# Toggle the configuration back on, and do more transactions.
$standby->append_conf('postgresql.conf', 'track_commit_timestamp = on');
$standby->restart;
for my $i (21 .. 30)
{
	$standby->safe_psql('postgres', "create table t$i()");
}

# Reconfigure the new standby and start it so as recovery is able to
# go through with a setting of max_worker_processes much larger than
# the primary.
$master->append_conf('postgresql.conf', 'max_worker_processes = 50');
$master->start;
my $standby_lsn =
  $standby->safe_psql('postgres', 'select pg_current_wal_lsn()');
$master->poll_query_until('postgres',
    qq{SELECT '$standby_lsn'::pg_lsn <= pg_last_wal_replay_lsn()});

# And finally check that commit timestamps are generated on the
# new primary.
my $standby_ts = $standby->safe_psql('postgres',
	qq{SELECT ts.* FROM pg_class, pg_xact_commit_timestamp(xmin) AS ts WHERE relname = 't30'}
);
isnt($standby_ts, '',
	"standby gives valid non-empty value after promotion");

$standby_ts = $standby->safe_psql('postgres',
	qq{SELECT ts.* FROM pg_class, pg_xact_commit_timestamp(xmin) AS ts WHERE relname = 't11'}
);
is($standby_ts, '',
	"standby gives valid empty value after promotion");
