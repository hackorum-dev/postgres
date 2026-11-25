
# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Tests to cross-check the consistency of GUC parameters with
# postgresql.conf.sample.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# Grab the names of all the parameters that can be listed in the
# configuration sample file.  config_file is an exception, it is not
# in postgresql.conf.sample but is part of the lists from guc_tables.c.
# Custom GUCs loaded by extensions are excluded.
my $all_params = $node->safe_psql(
	'postgres',
	"SELECT name
     FROM pg_settings
   WHERE NOT 'NOT_IN_SAMPLE' = ANY (pg_settings_get_flags(name)) AND
       name <> 'config_file' AND category <> 'Customized Options'
     ORDER BY 1");
# Note the lower-case conversion, for consistency.
my @all_params_array = split("\n", lc($all_params));

# Grab the names of all parameters marked as NOT_IN_SAMPLE.
my $not_in_sample = $node->safe_psql(
	'postgres',
	"SELECT name
     FROM pg_settings
   WHERE 'NOT_IN_SAMPLE' = ANY (pg_settings_get_flags(name))
     ORDER BY 1");
my @not_in_sample_array = split("\n", lc($not_in_sample));

# use the sample file from the temp install
my $share_dir = $node->config_data('--sharedir');
my $sample_file = "$share_dir/postgresql.conf.sample";

# List of all the GUCs found in the sample file.
my @gucs_in_file;

# List of all lines with tabs in the sample file.
my @lines_with_tabs;

# Read the sample file line-by-line, checking its contents to build a list
# of everything known as a GUC.
my $num_tests = 0;
my $line_num = 0;
open(my $contents, '<', $sample_file)
  || die "Could not open $sample_file: $!";
while (my $line = <$contents>)
{
	$line_num++;
	push @lines_with_tabs, $line_num
	  if $line =~ /\t/;

	# Check if this line matches a GUC parameter:
	# - Each parameter is preceded by "#", but not "# " in the sample
	# file.
	# - Valid configuration options are followed immediately by " = ",
	# with one space before and after the equal sign.
	if ($line =~ m/^#([_[:alnum:]]+) = .*/)
	{
		# Lower-case conversion matters for some of the GUCs.
		my $param_name = lc($1);

		# Ignore some exceptions.
		next if $param_name eq "include";
		next if $param_name eq "include_dir";
		next if $param_name eq "include_if_exists";

		# Update the list of GUCs found in the sample file, for the
		# follow-up tests.
		push @gucs_in_file, $param_name;

		next;
	}
	# Make sure each line starts with either a # or whitespace
	fail("$line missing initial # in postgresql.conf.sample")
	  if $line =~ /^\s*[^#\s]/;
}

close $contents;

# Cross-check that all the GUCs found in the sample file match the ones
# fetched above.  This maps the arrays to a hash, making the creation of
# each exclude and intersection list easier.
my %gucs_in_file_hash = map { $_ => 1 } @gucs_in_file;
my %all_params_hash = map { $_ => 1 } @all_params_array;
my %not_in_sample_hash = map { $_ => 1 } @not_in_sample_array;

my @missing_from_file = grep(!$gucs_in_file_hash{$_}, @all_params_array);
is(scalar(@missing_from_file),
	0, "no parameters missing from postgresql.conf.sample");

my @missing_from_list = grep(!$all_params_hash{$_}, @gucs_in_file);
is(scalar(@missing_from_list), 0, "no parameters missing from guc_tables.c");

my @sample_intersect = grep($not_in_sample_hash{$_}, @gucs_in_file);
is(scalar(@sample_intersect),
	0, "no parameters marked as NOT_IN_SAMPLE in postgresql.conf.sample");

is(scalar(@lines_with_tabs),
	0, "no lines with tabs in postgresql.conf.sample");

# These would log some information only on errors.
foreach my $param (@missing_from_file)
{
	print(
		"found GUC $param in guc_tables.c, missing from postgresql.conf.sample\n"
	);
}
foreach my $param (@missing_from_list)
{
	print(
		"found GUC $param in postgresql.conf.sample, with incorrect info in guc_tables.c\n"
	);
}
foreach my $param (@sample_intersect)
{
	print(
		"found GUC $param in postgresql.conf.sample, marked as NOT_IN_SAMPLE\n"
	);
}
foreach my $param (@lines_with_tabs)
{
	print("found tab in line $param in postgresql.conf.sample\n");
}

# Auto-tuned PGC_POSTMASTER parameters explicitly set to their auto-tuning
# value in the configuration file must not be reported as changed when the
# file is re-read without changes.  Querying pg_file_settings re-reads the
# file in the current session, so check both views in the same session.
my $autotuned_params =
	"'commit_timestamp_buffers', 'io_max_concurrency', "
  . "'subtransaction_buffers', 'transaction_buffers', 'wal_buffers'";
$node->append_conf(
	'postgresql.conf', qq(
commit_timestamp_buffers = 0
io_max_concurrency = -1
subtransaction_buffers = 0
transaction_buffers = 0
wal_buffers = -1
));
$node->restart;

my $not_applied = $node->safe_psql(
	'postgres',
	"SELECT coalesce(string_agg(name, ', ' ORDER BY name), 'none')
     FROM pg_file_settings
   WHERE name IN ($autotuned_params) AND NOT applied");
my $pending_restart = $node->safe_psql(
	'postgres',
	"SELECT count(*) FROM pg_file_settings;
     SELECT coalesce(string_agg(name, ', ' ORDER BY name), 'none')
     FROM pg_settings
   WHERE name IN ($autotuned_params) AND pending_restart");
# Only keep the output of the last query.
$pending_restart = (split("\n", $pending_restart))[-1];

is($not_applied, 'none',
	'auto-tuned parameters set to auto-tune are applied in pg_file_settings');
is($pending_restart, 'none',
	'auto-tuned parameters set to auto-tune are not pending restart');

done_testing();
