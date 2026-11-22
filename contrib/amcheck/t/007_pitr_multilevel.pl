# A multi-level btree page deletion interrupted by recovery between its
# MARK_PAGE_HALFDEAD record and the first of its UNLINK_PAGE records leaves
# a crash-consistent index: the subtree parent's leftmost pivot is already
# redirected, while the live internal subtree chain-top still sits to the
# left.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# origin: index with ~one entry per page, then DELETE the left half so
# VACUUM's index cleanup deletes a multi-level subtree at the leftmost edge.
my $origin = PostgreSQL::Test::Cluster->new('origin');
$origin->init(has_archiving => 1, allows_streaming => 1);
$origin->append_conf('postgresql.conf', 'autovacuum = off');
$origin->start;
$origin->backup('my_backup');
my $setup = <<EOSQL;
CREATE EXTENSION amcheck;
CREATE EXTENSION pg_walinspect;
CREATE TABLE no_index_cleanup (i INT, t TEXT);
CREATE INDEX no_index_cleanup_idx ON no_index_cleanup(t);
ALTER TABLE no_index_cleanup ALTER COLUMN t SET STORAGE EXTERNAL;
INSERT INTO no_index_cleanup
  VALUES (generate_series(1,30), repeat('1234567890',269));
VACUUM (FULL TRUE) no_index_cleanup;
EOSQL
$origin->safe_psql('postgres', $setup);
my $before = $origin->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
$origin->safe_psql('postgres',
	'DELETE FROM no_index_cleanup WHERE i < 15; VACUUM no_index_cleanup;');

# Target LSN: the start of the UNLINK_PAGE record directly following a
# multi-level MARK_PAGE_HALFDEAD (topparent is a real block, not
# InvalidBlockNumber = 4294967295).
my $records = $origin->safe_psql(
	'postgres', "
	SELECT start_lsn::text || '|' || record_type || '|' ||
	       coalesce(description, '')
	FROM pg_get_wal_records_info('$before', 'FFFFFFFF/FFFFFFFF')
	WHERE resource_manager = 'Btree'
	  AND record_type IN ('MARK_PAGE_HALFDEAD', 'UNLINK_PAGE')
	ORDER BY start_lsn");
my ($unlink_lsn, $prev_type, $prev_desc);
for my $line (split /\n/, $records)
{
	my ($lsn, $type, $desc) = split /\|/, $line, 3;
	$unlink_lsn = $lsn
	  if $type eq 'UNLINK_PAGE'
	  and $prev_type eq 'MARK_PAGE_HALFDEAD'
	  and $prev_desc =~ /topparent: (\d+)/
	  and $1 != 4294967295;
	last if $unlink_lsn;
	($prev_type, $prev_desc) = ($type, $desc // '');
}
$origin->stop;
die "did not find a multi-level MARK_PAGE_HALFDEAD -> UNLINK_PAGE pair"
  unless $unlink_lsn;

# replica: recover to just before that record -- the MARK record (which ends
# exactly there) is applied, none of the subtree's UNLINK records are.  That
# is the state a crash at that instant would leave.
my $replica = PostgreSQL::Test::Cluster->new('replica');
$replica->init_from_backup($origin, 'my_backup', has_restoring => 1);
$replica->append_conf('postgresql.conf', "recovery_target_lsn = '$unlink_lsn'");
$replica->append_conf('postgresql.conf', 'recovery_target_inclusive = off');
$replica->append_conf('postgresql.conf', 'recovery_target_action = promote');
$replica->start;
$replica->poll_query_until('postgres', "SELECT pg_is_in_recovery() = 'f';")
  or die "Timed out while waiting for PITR promotion";

# The state is benign: heap intact, healed by one VACUUM.  amcheck must accept
# it without VACUUM first -- the failing assertions below are the bug.
is($replica->safe_psql('postgres',
		'SELECT count(*) FROM no_index_cleanup'),
	'16', "rows intact at the interrupted deletion");
my ($rc, $stderr);
$rc = $replica->psql(
	'postgres',
	"SELECT bt_index_parent_check('no_index_cleanup_idx', true)",
	stderr => \$stderr);
print STDERR $stderr, "\n" if $rc != 0;
is($rc, 0, "bt_index_parent_check passes at the interrupted deletion");
unlike(
	$stderr,
	qr/first child of leftmost target page is not leftmost of its level/,
	"no XX002 leftmost finding");
$replica->safe_psql('postgres', 'VACUUM no_index_cleanup');
$rc = $replica->psql(
	'postgres',
	"SELECT bt_index_parent_check('no_index_cleanup_idx', true)",
	stderr => \$stderr);
is($rc, 0, "bt_index_parent_check passes after the healing VACUUM");

done_testing();
