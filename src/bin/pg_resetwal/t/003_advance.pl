use strict;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Basename;

#
# Check whether we can set arbitrarily large values for m,o,x options
#

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init();
$node->start();

my $data_dir = $node->data_dir;

# Run the regression tests
sub run_regression
{
	my $dlpath = dirname($ENV{REGRESS_SHLIB});
	my $pgregress = $ENV{PG_REGRESS};
	my $outputdir = $PostgreSQL::Test::Utils::tmp_check;

	my $rc =
	  system($ENV{PG_REGRESS}
	  	  . " "
		  . "--dlpath=\"$dlpath\" "
		  . "--bindir= "
		  . "--host="
		  . $node->host . " "
		  . "--port="
		  . $node->port . " "
		  . "--schedule=$dlpath/parallel_schedule "
		  . "--max-concurrent-tests=20 "
		  . "--inputdir=\"$dlpath\" "
		  . "--outputdir=\"$outputdir\"");
	if ($rc != 0)
	{
		# Dump out the regression diffs file, if there is one
		my $diffs = "$outputdir/regression.diffs";
		if (-e $diffs)
		{
			print "=== dumping $diffs ===\n";
			print slurp_file($diffs);
			print "=== EOF ===\n";
		}
	}
	is($rc, 0, 'regression tests pass');
}

#
# Test -x option
#

$node->safe_psql('postgres', q(
	CREATE TABLE test (
		int_data  INT
	);
	INSERT INTO test SELECT generate_series(1, 1000);
	BEGIN;
	DROP TABLE test;
	ABORT;
));

my $last_xid = $node->safe_psql('postgres', q( SELECT txid_current(); ));

# Advance next xid so that it doesn't fit on existing slru segment
my $next_xid = 0;
for (my $count = 1; $count < 20; $count++)
{
	$next_xid += 500_000_000;

	$node->stop();
	system_or_bail("pg_resetwal -D $data_dir -x $next_xid");
	$node->start();

	$node->safe_psql('postgres', q(
		VACUUM FREEZE;
	));
}

# Check whether postgres recognized statuses of all previous transactions
# correctly
my $tuples_num = $node->safe_psql('postgres', q(
	SELECT COUNT(*) FROM test;
));
ok($tuples_num == 1000, "we can see table 'test' and all tuples in it");

#
# Test -o option
#

my $next_oid = 100_000;

$node->stop();
system_or_bail("pg_resetwal -D $data_dir -o $next_oid");
$node->start();

$node->safe_psql('postgres', q(
	CREATE TABLE test1 (
		int_data INT
	);
));

my $advanced_oid = $node->safe_psql('postgres', q(
	SELECT oid FROM pg_class WHERE relname = 'test1';
));
ok($advanced_oid >= $next_oid, "oid was advanced succesfully");

#
# Test -m option
#

# Advance next multi xid so that it doesn't fit on existing slru segment
my $next_mxid = 4_000_000;
my $oldest_mxid = 100;

$node->stop();
system_or_bail("pg_resetwal -D $data_dir -m $next_mxid,$oldest_mxid");
$node->start();

# Check whether all works properly
$node->safe_psql('postgres', q(
	CREATE TABLE test2 (
		int_data INT
	);
	INSERT INTO test2 SELECT generate_series(1, 1000);
));

#
# Run regression tests to make sure that postgres is working normally after all
# manipulatons
#
run_regression();

$node->stop();
done_testing();
