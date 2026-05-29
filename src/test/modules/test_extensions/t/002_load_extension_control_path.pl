# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that a LOAD command with a hardcoded '$libdir/' prefix issued from
# within an extension script honors the library search path, so that
# extensions located via extension_control_path can be loaded.  This mirrors
# what e.g. PostGIS does in its upgrade scripts ("LOAD '$libdir/postgis-3'").
#
# A LOAD issued directly by a user must keep the literal '$libdir/' prefix and
# is therefore not affected (see commit f777d773878 / bug #18920).

use strict;
use warnings FATAL => 'all';
use File::Copy;
use File::Path qw(mkpath);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Make sure the test_ext shared library path is provided.
my $ext_lib_so = $ENV{TEST_EXT_LIB}
  or die "couldn't get the test_ext shared library path";

my $ext_name = "test_ext";

# Create the custom extension directory layout:
#   $ext_dir/extension/  -- .control and .sql files
#   $ext_dir/lib/        -- .so file
my $ext_dir = PostgreSQL::Test::Utils::tempdir();
mkpath("$ext_dir/extension");
mkpath("$ext_dir/lib");
my $ext_lib = "$ext_dir/lib";

# Copy the .so file into the lib/ subdirectory.
copy($ext_lib_so, $ext_lib)
  or die "could not copy '$ext_lib_so' to '$ext_lib': $!";

create_extension_files($ext_name, $ext_dir);

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;

# Use the correct separator and escape '\' when running on Windows.
my $sep = $windows_os ? ";" : ":";
my $ext_path = $windows_os ? ($ext_dir =~ s/\\/\\\\/gr) : $ext_dir;
my $ext_lib_path = $windows_os ? ($ext_lib =~ s/\\/\\\\/gr) : $ext_lib;

# Configure extension_control_path so the .control file is found in our
# extension/ directory, and dynamic_library_path so the .so is found in lib/.
$node->append_conf(
	'postgresql.conf', qq(
extension_control_path = '\$system$sep$ext_path'
dynamic_library_path = '\$libdir$sep$ext_lib_path'
));

$node->start;

# CREATE EXTENSION runs the script, which contains a hardcoded
# "LOAD '\$libdir/test_ext'".  Before the fix this failed because the
# '\$libdir/' prefix was not stripped for LOAD, so the library search path
# (and thus the custom lib/ directory) was never consulted.
$node->safe_psql('postgres', "CREATE EXTENSION $ext_name");

# The function added by the extension exercises the function-load path, which
# already stripped '$libdir/'.
my ($code, $stdout, $stderr) = $node->psql('postgres', 'SELECT test_ext()');
is($code, 0, 'extension function works');
like($stderr, qr/NOTICE:  running successful/, 'extension function loaded');

# A LOAD issued directly by a user (outside any extension script) must keep
# the literal '$libdir/' prefix, so it resolves to the package library
# directory and does not find the library installed in our custom lib/ dir.
($code, $stdout, $stderr) =
  $node->psql('postgres', "LOAD '\$libdir/$ext_name'");
isnt($code, 0, 'direct LOAD with $libdir prefix is not redirected to the path');
like(
	$stderr,
	qr{could not access file "\$libdir/$ext_name"},
	'direct LOAD keeps the literal $libdir prefix');

$node->stop;

# Write .control and .sql files into $ext_dir/extension/.
# The script uses a hardcoded "LOAD '$libdir/...'" and a '$libdir/' prefixed
# module_pathname to reproduce what most extensions do by default.
sub create_extension_files
{
	my ($ext_name, $ext_dir) = @_;

	open my $cf, '>', "$ext_dir/extension/$ext_name.control"
	  or die "could not create control file: $!";
	print $cf "comment = 'Test C extension for extension_control_path + LOAD'\n";
	print $cf "default_version = '1.0'\n";
	print $cf "module_pathname = '\$libdir/$ext_name'\n";
	print $cf "relocatable = true\n";
	close $cf;

	open my $sqlf, '>', "$ext_dir/extension/$ext_name--1.0.sql"
	  or die "could not create SQL file: $!";
	print $sqlf "/* $ext_name--1.0.sql */\n";
	print $sqlf
	  "-- complain if script is sourced in psql, rather than via CREATE EXTENSION\n";
	print $sqlf
	  qq'\\echo Use "CREATE EXTENSION $ext_name" to load this file. \\quit\n';
	print $sqlf "LOAD '\$libdir/$ext_name';\n";
	print $sqlf "CREATE FUNCTION test_ext()\n";
	print $sqlf "RETURNS void AS 'MODULE_PATHNAME'\n";
	print $sqlf "LANGUAGE C;\n";
	close $sqlf;
}

done_testing();
