# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('sampling');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'pg_stat_statements'");
$node->append_conf('postgresql.conf', "compute_query_id = on");
$node->append_conf('postgresql.conf', "pg_stat_statements.track = 'all'");
$node->start;

# The other extension also requests query instrumentation.  Its presence must
# not make auto_explain log a cursor that auto_explain did not sample.
for my $end ('commit', 'close')
{
	for my $kind ('control', 'sampled', 'unsampled')
	{
		my $name = "${kind}_${end}";
		my $cursor = "pg19877_$name";
		my $initial = $kind eq 'unsampled' ? 0 : 1;
		my $later = $kind eq 'sampled' ? 0 : 1;
		my $sql = qq{
LOAD 'auto_explain';
SET client_min_messages = notice;
SET auto_explain.log_level = notice;
SET auto_explain.log_min_duration = 0;
SET auto_explain.log_analyze = off;
SET auto_explain.log_format = text;
SET auto_explain.log_nested_statements = off;
SET auto_explain.sample_rate = $initial;
BEGIN;
DECLARE $cursor CURSOR FOR SELECT 101 AS pg19877_value;
FETCH ALL FROM $cursor;
};

		# A separate statement replaces the backend-global sampling decision.
		if ($kind ne 'control')
		{
			$sql .= qq{
SET auto_explain.sample_rate = $later;
SELECT 202 AS pg19877_resampler;
};
		}
		$sql .= "CLOSE $cursor;\n" if $end eq 'close';
		$sql .= "COMMIT;\n";

		my ($stdout, $stderr);
		my $ret = $node->psql('postgres', $sql,
			stdout => \$stdout, stderr => \$stderr);
		is($ret, 0, "$name executes successfully");
		like($stdout, qr/^101$/m, "$name returns its cursor row");
		my $plan = qr/Query Text:\s*DECLARE\s+\Q$cursor\E\s+CURSOR\b/;
		if ($kind eq 'unsampled')
		{
			unlike($stderr, $plan, "$name is not logged by auto_explain");
		}
		else
		{
			like($stderr, $plan, "$name keeps original sampling decision");
		}
	}
}

$node->stop;
done_testing();
