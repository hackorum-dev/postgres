# Copyright (c) 2025, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';
use File::Copy;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

# This tests scenarios related to the service name and the service file,
# for the connection options and their environment variables.

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->start;

# Set up a dummy node used for the connection tests, but do not start it.
# This ensures that the environment variables used for the connection do
# not interfere with the connection attempts, and that the service file's
# contents are used.
my $dummy_node = PostgreSQL::Test::Cluster->new('dummy_node');
$dummy_node->init;

my $td = PostgreSQL::Test::Utils::tempdir;

# Create the set of service files used in the tests.
# File that includes a valid service name, and uses a decomposed connection
# string for its contents, split on spaces.
my $srvfile_valid = "$td/pg_service_valid.conf";
append_to_file(
	$srvfile_valid, qq{
# Settings without a section are, historically, ignored.
host=256.256.256.256
port=1
unknown-setting=1

[my_srv]
});
foreach my $param (split(/\s+/, $node->connstr))
{
	append_to_file($srvfile_valid, $param . "\n");
}

# File defined with no contents, used as default value for PGSERVICEFILE,
# so as no lookup is attempted in the user's home directory.
my $srvfile_empty = "$td/pg_service_empty.conf";
append_to_file($srvfile_empty, '');

# Default service file in PGSYSCONFDIR.
my $srvfile_default = "$td/pg_service.conf";

# Missing service file.
my $srvfile_missing = "$td/pg_service_missing.conf";

# Service file with nested "service" defined.
my $srvfile_nested = "$td/pg_service_nested.conf";
copy($srvfile_valid, $srvfile_nested)
  or die "Could not copy $srvfile_valid to $srvfile_nested: $!";
append_to_file($srvfile_nested, "service=invalid_srv\n");

# File with an unknown setting.
my $srvfile_bad_keyword = "$td/pg_service_bad_keyword.conf";
append_to_file(
	$srvfile_bad_keyword, qq{
[my_srv]
bad-unknown-setting=1
});

# Service file with nested "servicefile" defined.
my $srvfile_nested_2 = "$td/pg_service_nested_2.conf";
copy($srvfile_valid, $srvfile_nested_2)
  or die "Could not copy $srvfile_valid to $srvfile_nested_2: $!";
append_to_file($srvfile_nested_2, "servicefile=$srvfile_default\n");

# Set the fallback directory lookup of the service file to the temporary
# directory of this test.  PGSYSCONFDIR is used if the service file
# defined in PGSERVICEFILE cannot be found, or when a service file is
# found but not the service name.
local $ENV{PGSYSCONFDIR} = $td;
# Force PGSERVICEFILE to a default location, so as this test never
# tries to look at a home directory.  This value needs to remain
# at the top of this script before running any tests, and should never
# be changed.
local $ENV{PGSERVICEFILE} = "$srvfile_empty";

# Checks combinations of service name and a valid service file.
{
	local $ENV{PGSERVICEFILE} = $srvfile_valid;
	$dummy_node->connect_ok(
		'service=my_srv',
		'connection with correct "service" string and PGSERVICEFILE',
		sql => "SELECT 'connect1_1'",
		expected_stdout => qr/connect1_1/);

	$dummy_node->connect_ok(
		'postgres://?service=my_srv',
		'connection with correct "service" URI and PGSERVICEFILE',
		sql => "SELECT 'connect1_2'",
		expected_stdout => qr/connect1_2/);

	$dummy_node->connect_fails(
		'service=undefined-service',
		'connection with incorrect "service" string and PGSERVICEFILE',
		expected_stderr =>
		  qr/definition of service "undefined-service" not found/);

	local $ENV{PGSERVICE} = 'my_srv';
	$dummy_node->connect_ok(
		'',
		'connection with correct PGSERVICE and PGSERVICEFILE',
		sql => "SELECT 'connect1_3'",
		expected_stdout => qr/connect1_3/);

	local $ENV{PGSERVICE} = 'undefined-service';
	$dummy_node->connect_fails(
		'',
		'connection with incorrect PGSERVICE and PGSERVICEFILE',
		expected_stdout =>
		  qr/definition of service "undefined-service" not found/);
}

{
	# Check handling of the defaults section.
	#
	# pg_service_defaults.conf contains the same parameters as srvfile_valid,
	# but it uses a default section to select the service automatically. (Use of
	# a service remains necessary, to take precedence over Test::Cluster's
	# automatic envvars.)
	my $srvfile_defaults = "$td/pg_service_defaults.conf";
	append_to_file(
		$srvfile_defaults, qq{
# Settings before the default section must be ignored.
host=256.256.256.256
port=1
unknown-setting=1

[default]
+=defaults
service=my_srv
options=-O
?unknown-setting=1  # should be ignored

[my_srv]
});
	foreach my $param (split(/\s+/, $node->connstr))
	{
		append_to_file($srvfile_defaults, $param . "\n");
	}

	local $ENV{PGSERVICEFILE} = $srvfile_defaults;
	$dummy_node->connect_ok(
		'',
		'connection with dynamic defaults in PGSERVICEFILE',
		sql => 'SHOW allow_system_table_mods',
		expected_stdout => qr/on/);

	# TODO is it really okay that postgres:// means whatever the environment
	# says it means...?
	$dummy_node->connect_ok(
		'postgres://',
		'connection with empty URI, dynamic defaults, and PGSERVICEFILE',
		sql => 'SHOW allow_system_table_mods',
		expected_stdout => qr/on/);

	$dummy_node->connect_fails(
		'service=default',
		'default sections may not be selected via connection parameter',
		expected_stderr =>
		  qr/default section \[default\] may not be named as a service/);

	{
		local $ENV{PGSERVICE} = 'default';
		$dummy_node->connect_fails(
			'',
			'default sections may not be selected via PGSERVICE',
			expected_stderr =>
			  qr/default section \[default\] may not be named as a service/);
	}

	# A file containing more than one default section is rejected.
	my $srvfile_too_many_defaults = "$td/pg_service_too_many_defaults.conf";
	copy($srvfile_defaults, $srvfile_too_many_defaults)
	  or die
	  "Could not copy $srvfile_defaults to $srvfile_too_many_defaults: $!";
	append_to_file(
		$srvfile_too_many_defaults, qq{
[default]
+=defaults
});

	local $ENV{PGSERVICEFILE} = $srvfile_too_many_defaults;
	$dummy_node->connect_fails(
		'',
		'service files may not contain more than one default section',
		expected_stderr => qr/only the first section may be marked default/);

	# A default section may not come after the first service section.
	my $srvfile_defaults_after_service =
	  "$td/pg_service_defaults_after_service.conf";
	copy($srvfile_valid, $srvfile_defaults_after_service)
	  or die
	  "Could not copy $srvfile_valid to $srvfile_defaults_after_service: $!";
	append_to_file(
		$srvfile_defaults_after_service, qq{
[default]
+=defaults
});

	local $ENV{PGSERVICEFILE} = $srvfile_defaults_after_service;
	$dummy_node->connect_fails(
		'',
		'defaults section must be first in the file',
		expected_stderr => qr/only the first section may be marked default/);
}

# Checks case of incorrect service file.
{
	local $ENV{PGSERVICEFILE} = $srvfile_missing;
	$dummy_node->connect_fails(
		'service=my_srv',
		'connection with correct "service" string and incorrect PGSERVICEFILE',
		expected_stderr =>
		  qr/service file ".*pg_service_missing.conf" not found/);
}

# Checks case of service file named "pg_service.conf" in PGSYSCONFDIR.
{
	# Create copy of valid file
	my $srvfile_default = "$td/pg_service.conf";
	copy($srvfile_valid, $srvfile_default);

	$dummy_node->connect_ok(
		'service=my_srv',
		'connection with correct "service" string and pg_service.conf',
		sql => "SELECT 'connect2_1'",
		expected_stdout => qr/connect2_1/);

	$dummy_node->connect_ok(
		'postgres://?service=my_srv',
		'connection with correct "service" URI and default pg_service.conf',
		sql => "SELECT 'connect2_2'",
		expected_stdout => qr/connect2_2/);

	$dummy_node->connect_fails(
		'service=undefined-service',
		'connection with incorrect "service" string and default pg_service.conf',
		expected_stderr =>
		  qr/definition of service "undefined-service" not found/);

	local $ENV{PGSERVICE} = 'my_srv';
	$dummy_node->connect_ok(
		'',
		'connection with correct PGSERVICE and default pg_service.conf',
		sql => "SELECT 'connect2_3'",
		expected_stdout => qr/connect2_3/);

	local $ENV{PGSERVICE} = 'undefined-service';
	$dummy_node->connect_fails(
		'',
		'connection with incorrect PGSERVICE and default pg_service.conf',
		expected_stdout =>
		  qr/definition of service "undefined-service" not found/);

	# Remove default pg_service.conf.
	unlink($srvfile_default);
}

# Checks nested service file contents.
{
	local $ENV{PGSERVICEFILE} = $srvfile_nested;

	$dummy_node->connect_fails(
		'service=my_srv',
		'connection with "service" in nested service file',
		expected_stderr =>
		  qr/nested "service" specifications not supported in service file/);

	local $ENV{PGSERVICEFILE} = $srvfile_nested_2;

	$dummy_node->connect_fails(
		'service=my_srv',
		'connection with "servicefile" in nested service file',
		expected_stderr =>
		  qr/nested "servicefile" specifications not supported in service file/
	);
}

# Check behavior with unknown keywords.
{
	local $ENV{PGSERVICEFILE} = $srvfile_bad_keyword;

	$dummy_node->connect_fails(
		'service=my_srv',
		'connection with unknown connection option in service',
		expected_stderr =>
		  qr/unknown keyword "bad-unknown-setting" in service file/);
}

# Properly escape backslashes in the path, to ensure the generation of
# correct connection strings.
my $srvfile_win_cared = $srvfile_valid;
$srvfile_win_cared =~ s/\\/\\\\/g;

# Checks that the "servicefile" option works as expected
{
	$dummy_node->connect_ok(
		q{service=my_srv servicefile='} . $srvfile_win_cared . q{'},
		'connection with valid servicefile in connection string',
		sql => "SELECT 'connect3_1'",
		expected_stdout => qr/connect3_1/);

	# Encode slashes and backslash
	my $encoded_srvfile = $srvfile_valid =~ s{([\\/])}{
		$1 eq '/' ? '%2F' : '%5C'
	}ger;

	# Additionally encode a colon in servicefile path of Windows
	$encoded_srvfile =~ s/:/%3A/g;

	$dummy_node->connect_ok(
		'postgresql:///?service=my_srv&servicefile=' . $encoded_srvfile,
		'connection with valid servicefile in URI',
		sql => "SELECT 'connect3_2'",
		expected_stdout => qr/connect3_2/);

	local $ENV{PGSERVICE} = 'my_srv';
	$dummy_node->connect_ok(
		q{servicefile='} . $srvfile_win_cared . q{'},
		'connection with PGSERVICE and servicefile in connection string',
		sql => "SELECT 'connect3_3'",
		expected_stdout => qr/connect3_3/);

	$dummy_node->connect_ok(
		'postgresql://?servicefile=' . $encoded_srvfile,
		'connection with PGSERVICE and servicefile in URI',
		sql => "SELECT 'connect3_4'",
		expected_stdout => qr/connect3_4/);
}

# Check that the "servicefile" option takes priority over the PGSERVICEFILE
# environment variable.
{
	local $ENV{PGSERVICEFILE} = 'non-existent-file.conf';

	$dummy_node->connect_fails(
		'service=my_srv',
		'connection with invalid PGSERVICEFILE',
		expected_stderr =>
		  qr/service file "non-existent-file\.conf" not found/);

	$dummy_node->connect_ok(
		q{service=my_srv servicefile='} . $srvfile_win_cared . q{'},
		'connection with both servicefile and PGSERVICEFILE',
		sql => "SELECT 'connect4_1'",
		expected_stdout => qr/connect4_1/);
}

$node->teardown_node;

done_testing();
