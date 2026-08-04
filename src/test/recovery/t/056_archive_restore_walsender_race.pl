# KeepFileRestoredFromArchive must not unlink before durable_rename
# or a concurrent walsender can see a missing segment

use strict;
use warnings FATAL => 'all';
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Windows already renames aside under FILE_SHARE_DELETE so the Unix race differs
plan skip_all => 'Unix-specific race'
  if $PostgreSQL::Test::Utils::windows_os;
plan skip_all => 'injection points required'
  unless $ENV{enable_injection_points} eq 'yes';

my $inj = 'keepfile-restored-before-rename';
my $gate = "$PostgreSQL::Test::Utils::tmp_check/g$$";

# Switch WAL and wait until the archiver has finished each segment
sub advance_wal_and_wait_archive
{
	my ($node, $num) = @_;

	for (1 .. $num)
	{
		my $seg = $node->safe_psql('postgres',
			'SELECT pg_walfile_name(pg_current_wal_lsn())');
		$node->safe_psql(
			'postgres', q{
			SELECT pg_logical_emit_message(false, '', 'foo');
			SELECT pg_switch_wal();
			});
		$node->poll_query_until('postgres',
			"SELECT '$seg' <= last_archived_wal FROM pg_stat_archiver")
		  or die "timed out waiting for $seg to be archived";
	}
}

my $p = PostgreSQL::Test::Cluster->new('p');
$p->init(
	has_archiving => 1, allows_streaming => 1,
	extra => ['--wal-segsize', '1']);
$p->append_conf('postgresql.conf',
	"shared_preload_libraries = 'injection_points'");
$p->start;
$p->safe_psql('postgres', 'CREATE EXTENSION injection_points');
advance_wal_and_wait_archive($p, 2);
$p->backup('b');
advance_wal_and_wait_archive($p, 3);

my $a = $p->archive_dir;
# Latest archived segment is forced through KeepFileRestoredFromArchive
my $w = (sort grep { /^[0-9A-F]{24}$/ } slurp_dir($a))[-1];
my $prev = substr($w, 0, 16) . sprintf('%08X', hex(substr($w, 16, 8)) - 1);
my $segno = hex(substr($w, 16, 8));
my $w_lsn = sprintf('%X/%08X', ($segno * 1048576) >> 32,
	($segno * 1048576) & 0xFFFFFFFF);

# Copy the target WAL segment into pg_wal first so restore must replace it
# Gate restore of that segment until the injection point is attached before rename
my $s = PostgreSQL::Test::Cluster->new('s');
$s->init_from_backup($p, 'b', has_restoring => 1);
system('cp', "$a/$w", $s->data_dir . "/pg_wal/$w") == 0 or die $!;
$s->append_conf('postgresql.conf', qq{
recovery_prefetch = off
shared_preload_libraries = 'injection_points'
restore_command = 'case "%f" in $w) while [ ! -f $gate ]; do sleep 0.01; done ;; esac; cp "$a/%f" "%p"'
});
$s->start;
$s->wait_for_log(qr/restored log file "\Q$prev\E" from archive/);
$s->safe_psql('postgres', "SELECT injection_points_attach('$inj','wait')");
system('touch', $gate);
$s->wait_for_event('startup', $inj);

# The preplaced segment must still be present at this point before rename
my $w_path = $s->data_dir . "/pg_wal/$w";
ok(-e $w_path, 'segment still present before rename');

# Open a real walsender on the standby while paused in that window
my $stream_dir = PostgreSQL::Test::Utils::tempdir_short();
my $recv = IPC::Run::start(
	[
		'pg_receivewal',
		'--dbname' => $s->connstr('postgres'),
		'--directory' => $stream_dir,
		'--verbose',
		'--no-loop',
	]);
$s->poll_query_until('postgres',
	q{SELECT count(*) > 0 FROM pg_stat_replication})
  or die "timed out waiting for walsender on standby";
pass('walsender started on standby during rename window');
$recv->signal('TERM');
$recv->finish;
rmtree($stream_dir);

# Standby still has the target segment so its walsender cannot see it missing
# Remove the same segment on the primary and START_REPLICATION at its first LSN
# to show the missing segment error a walsender would hit
my $p_w_path = $p->data_dir . "/pg_wal/$w";
ok(-e $p_w_path, 'primary still has the raced segment');
unlink($p_w_path) or die "could not unlink primary $w: $!";

my $logstart = -s $p->logfile;
$p->psql(
	'postgres',
	"START_REPLICATION PHYSICAL $w_lsn",
	replication => 1);
$p->wait_for_log(
	qr/requested WAL segment \Q$w\E has already been removed/,
	$logstart);
pass('walsender errors when the raced segment is missing');

# Put the segment back on the primary then wake startup to finish the rename
system('cp', "$a/$w", $p_w_path) == 0 or die $!;
$s->safe_psql('postgres', "SELECT injection_points_wakeup('$inj')");
$s->poll_query_until(
	'postgres',
	qq{SELECT count(*) = 0 FROM pg_stat_activity
	   WHERE backend_type = 'startup'
	     AND wait_event = '$inj'})
  or die "timed out waiting for startup to leave $inj";
ok(-e $w_path, 'restored segment present after rename');

done_testing();
