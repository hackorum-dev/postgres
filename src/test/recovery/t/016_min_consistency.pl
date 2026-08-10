
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test for checking consistency of on-disk pages for a cluster with
# the minimum recovery LSN, ensuring that the updates happen across
# all processes.  In this test, the updates from the startup process
# and the checkpointer (which triggers non-startup code paths) are
# both checked.
#
# The same setup is also used to check that a standby refuses read-only
# connections until replay has reached minRecoveryPoint after the postmaster
# has performed a crash reset.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

# Find the largest LSN in the set of pages part of the given relation
# file.  This is used for offline checks of page consistency.  The LSN
# is historically stored as a set of two numbers of 4 byte-length
# located at the beginning of each page.
sub find_largest_lsn
{
	my $blocksize = int(shift);
	my $filename = shift;
	my ($max_hi, $max_lo) = (0, 0);
	open(my $fh, "<:raw", $filename)
	  or die "failed to open $filename: $!";
	my ($buf, $len);
	while ($len = read($fh, $buf, $blocksize))
	{
		$len == $blocksize
		  or die "read only $len of $blocksize bytes from $filename";
		my ($hi, $lo) = unpack("LL", $buf);

		if ($hi > $max_hi or ($hi == $max_hi and $lo > $max_lo))
		{
			($max_hi, $max_lo) = ($hi, $lo);
		}
	}
	defined($len) or die "read error on $filename: $!";
	close($fh);

	return sprintf("%X/%08X", $max_hi, $max_lo);
}

# Initialize primary node
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);

# Set shared_buffers to a very low value to enforce discard and flush
# of PostgreSQL buffers on standby, enforcing other processes than the
# startup process to update the minimum recovery LSN in the control
# file.  Autovacuum is disabled so as there is no risk of having other
# processes than the checkpointer doing page flushes.
$primary->append_conf("postgresql.conf", <<EOF);
shared_buffers = 128kB
autovacuum = off
EOF

# Start the primary
$primary->start;

# setup/start a standby
$primary->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);

# restart_after_crash has to be turned back on, since Cluster->init disables
# it and the crash reset checked below depends on it.  A long checkpoint
# timeout keeps a restartpoint from advancing the control file's redo pointer
# before then; the explicit CHECKPOINT issued later still forces one.
$standby->append_conf('postgresql.conf', <<EOF);
restart_after_crash = on
checkpoint_timeout = 1h
EOF

$standby->start;

# Create base table whose data consistency is checked.
my $rows = 10000;
$primary->safe_psql(
	'postgres', "
CREATE TABLE test1 (a int) WITH (fillfactor = 10);
INSERT INTO test1 SELECT generate_series(1, $rows);");

# Take a checkpoint and enforce post-checkpoint full page writes
# which makes the startup process replay those pages, updating
# minRecoveryPoint.
$primary->safe_psql('postgres', 'CHECKPOINT;');
$primary->safe_psql('postgres', 'UPDATE test1 SET a = a + 1;');

# Wait for last record to have been replayed on the standby.
$primary->wait_for_catchup($standby);

# Fill in the standby's shared buffers with the data filled in
# previously.
$standby->safe_psql('postgres', 'SELECT count(*) FROM test1;');

# Update the table again, this does not generate full page writes so
# the standby will replay records associated with it, but the startup
# process will not flush those pages.
$primary->safe_psql('postgres', 'UPDATE test1 SET a = a + 1;');

# Extract from the relation the last block created and its relation
# file, this will be used at the end of the test for sanity checks.
my $blocksize = $primary->safe_psql('postgres',
	"SELECT setting::int FROM pg_settings WHERE name = 'block_size';");
my $last_block = $primary->safe_psql('postgres',
	"SELECT pg_relation_size('test1')::int / $blocksize - 1;");
my $relfilenode = $primary->safe_psql('postgres',
	"SELECT pg_relation_filepath('test1'::regclass);");

# Wait for last record to have been replayed on the standby.
$primary->wait_for_catchup($standby);

# ----------------------------------------------------------------------------
# Check that a crash reset does not let the standby accept connections early.
#
# No restartpoint has run yet, so the control file's redo pointer is still the
# one taken by the base backup, while minRecoveryPoint has followed the pages
# the standby has been evicting.  That gap is what a replacement startup
# process has to replay again before connections may be accepted.
# ----------------------------------------------------------------------------
my ($min_recovery_point, $redo_ptr, $gap) = split /\|/, $standby->safe_psql(
	'postgres', q{
SELECT (SELECT min_recovery_end_lsn FROM pg_control_recovery()),
       (SELECT redo_lsn FROM pg_control_checkpoint()),
       (SELECT min_recovery_end_lsn FROM pg_control_recovery()) -
       (SELECT redo_lsn FROM pg_control_checkpoint())});

note "minRecoveryPoint $min_recovery_point, redo pointer $redo_ptr, "
  . "gap $gap bytes";

cmp_ok($gap, '>', 0,
	"minRecoveryPoint is ahead of the redo pointer in the control file");

# recovery_min_apply_delay makes the check below reliable rather than
# timing-dependent.  recoveryApplyDelay() does not apply the delay until
# consistency has been reached, so a correct standby ignores it and replays to
# minRecoveryPoint at full speed, whereas one that wrongly believes it is
# already consistent honours it and stops well short.
$standby->append_conf('postgresql.conf', 'recovery_min_apply_delay = 1h');
$standby->reload;

my $log_offset = -s $standby->logfile;

# Kill a live backend, which forces the crash reset and with it a replacement
# startup process.
my $victim = $standby->background_psql('postgres');
my $victim_pid = $victim->query_safe('SELECT pg_backend_pid()');
chomp $victim_pid;

PostgreSQL::Test::Utils::system_log('pg_ctl', 'kill', 'KILL', $victim_pid);

# The backend is gone, so psql exits with an error of its own; ignore it.
eval { $victim->quit };

# Once replay has restarted, the standby must not accept a connection until
# replay has reached minRecoveryPoint again.
$standby->wait_for_log(qr/redo starts at/, $log_offset);

my ($replay_lsn, $past_min_recovery_point, $behind);

my $deadline = time() + $PostgreSQL::Test::Utils::timeout_default;
while (time() < $deadline)
{
	my ($rc, $stdout, $stderr) = $standby->psql(
		'postgres', qq{
SELECT pg_last_wal_replay_lsn(),
       pg_last_wal_replay_lsn() >= '$min_recovery_point'::pg_lsn,
       '$min_recovery_point'::pg_lsn - pg_last_wal_replay_lsn()},
		on_error_stop => 0);

	if ($rc == 0 && $stdout ne '')
	{
		($replay_lsn, $past_min_recovery_point, $behind) = split /\|/,
		  $stdout;
		last;
	}

	usleep(100_000);
}

ok(defined $replay_lsn,
	"standby accepted a read-only connection after the crash reset")
  or BAIL_OUT("standby never accepted a connection after the crash reset");

note "first post-crash connection: replay $replay_lsn, "
  . "minRecoveryPoint $min_recovery_point, $behind bytes short";

is($past_min_recovery_point, 't',
	"standby replayed up to minRecoveryPoint ($min_recovery_point) before "
	  . "accepting connections (replay was at $replay_lsn, $behind bytes short)"
);

# Ask the same connection for data.  Only the column value is ever updated, so
# the row count is invariant; a standby that opened early answers from pages
# replay has not caught up with, and reports either a short count or no table
# at all, since its snapshot predates the transaction that created it.
my ($count_rc, $count, $count_err) =
  $standby->psql('postgres', 'SELECT count(*) FROM test1',
	on_error_stop => 0);
chomp($count, $count_err);
$count = "query failed: $count_err" if $count_err ne '';

is($count, $rows,
	"standby served correct data to its first post-crash connection");

# Let replay run at full speed again for the rest of the test.
$standby->append_conf('postgresql.conf', 'recovery_min_apply_delay = 0');
$standby->reload;
$primary->wait_for_catchup($standby);

# Issue a restart point on the standby now, which makes the checkpointer
# update minRecoveryPoint.
$standby->safe_psql('postgres', 'CHECKPOINT;');

# Now shut down the primary violently so as the standby does not
# receive the shutdown checkpoint, making sure that the startup
# process does not flush any pages on its side.  The standby is
# cleanly stopped, which makes the checkpointer update minRecoveryPoint
# with the restart point created at shutdown.
$primary->stop('immediate');
$standby->stop('fast');

# Check the data consistency of the instance while offline.  This is
# done by directly scanning the on-disk relation blocks and what
# pg_controldata lets know.
my $standby_data = $standby->data_dir;
my $offline_max_lsn =
  find_largest_lsn($blocksize, "$standby_data/$relfilenode");

# Fetch minRecoveryPoint from the control file itself
my ($stdout, $stderr) = run_command([ 'pg_controldata', $standby_data ]);
my @control_data = split("\n", $stdout);
my $offline_recovery_lsn = undef;
foreach (@control_data)
{
	if ($_ =~ /^Minimum recovery ending location:\s*(.*)$/mg)
	{
		$offline_recovery_lsn = $1;
		last;
	}
}
die "No minRecoveryPoint in control file found\n"
  unless defined($offline_recovery_lsn);

# minRecoveryPoint should never be older than the maximum LSN for all
# the pages on disk.
ok($offline_recovery_lsn ge $offline_max_lsn,
	"Check offline that table data is consistent with minRecoveryPoint");

done_testing();
