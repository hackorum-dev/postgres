# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that pg_upgrade --check reports stored settings the new cluster
# rejects.

use strict;
use warnings FATAL => 'all';

use File::Find qw(find);
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $old =
  PostgreSQL::Test::Cluster->new('old', install_path => $ENV{oldinstall});
$old->init;
$old->start;

my $have_param_acl = $old->pg_version >= 15;

# Settings that restore with only a warning about missing objects.
$old->safe_psql(
	'postgres', q{
	CREATE ROLE r_ok;
	ALTER ROLE r_ok SET default_tablespace = 'no_such_tablespace';
	ALTER ROLE r_ok SET myext.foo = 'bar';
	CREATE DATABASE d_ok;
	CREATE FUNCTION f_ok() RETURNS int LANGUAGE sql
	  SET work_mem = '2MB' SET search_path = public, pg_temp
	  RETURN 1;
	CREATE FUNCTION f_bad() RETURNS int LANGUAGE sql RETURN 1;
	CREATE ROLE r_bad;
	CREATE DATABASE d_bad;
});
$old->safe_psql(
	'd_ok', q{
	CREATE TEXT SEARCH CONFIGURATION public.mycfg (COPY = english);
	ALTER DATABASE d_ok SET default_text_search_config = 'public.mycfg';
});

# Bad settings, put straight into the catalogs.  The last three are not
# dumped and must not be reported.
$old->safe_psql(
	'postgres', q{
	SET allow_system_table_mods = on;
	INSERT INTO pg_db_role_setting VALUES
	  (0, 'r_bad'::regrole, '{no_such_setting=on}'),
	  ((SELECT oid FROM pg_database WHERE datname = 'd_bad'), 0,
	   '{standard_conforming_strings=off}'),
	  ((SELECT oid FROM pg_database WHERE datname = 'd_bad'), 'r_bad'::regrole,
	   '{default_toast_compression=no_such_method}'),
	  (0, 0, '{no_such_setting_all=on}'),
	  (0, 'pg_monitor'::regrole, '{no_such_setting_predefined=on}'),
	  ((SELECT oid FROM pg_database WHERE datname = 'template0'), 0,
	   '{no_such_setting_template0=on}');
	UPDATE pg_proc SET proconfig = '{standard_conforming_strings=off}'
	  WHERE proname = 'f_bad';
});

if ($have_param_acl)
{
	$old->safe_psql(
		'postgres', q{
		GRANT SET ON PARAMETER work_mem TO r_ok;
		GRANT SET ON PARAMETER myext.removed TO r_ok;
		SET allow_system_table_mods = on;
		UPDATE pg_parameter_acl SET parname = 'no_such_builtin'
		  WHERE parname = 'myext.removed';
	});
}

$old->stop;

my $new = PostgreSQL::Test::Cluster->new('new');
$new->init;

# In a VPATH build, we'll be started in the source directory, but we want
# to run pg_upgrade in the build directory so that any files generated finish
# in it, like delete_old_cluster.{sh,bat}.
chdir ${PostgreSQL::Test::Utils::tmp_check};

my @pg_upgrade_cmd = (
	'pg_upgrade', '--no-sync',
	'--old-datadir' => $old->data_dir,
	'--new-datadir' => $new->data_dir,
	'--old-bindir' => $old->config_data('--bindir'),
	'--new-bindir' => $new->config_data('--bindir'),
	'--socketdir' => $new->host,
	'--old-port' => $old->port,
	'--new-port' => $new->port,
	'--copy');

command_checks_all(
	[ @pg_upgrade_cmd, '--check' ],
	1,
	[
		qr/Checking for stored settings the new cluster rejects\s+fatal/,
		qr/stored settings that the new cluster does not accept/,
	],
	[qr//],
	'pg_upgrade --check fails on stored settings the new cluster rejects');

my $report;
find(
	sub {
		$report = $File::Find::name
		  if $File::Find::name =~ m/stored_settings\.txt/;
	},
	$new->data_dir . "/pg_upgrade_output.d");
my $content = slurp_file($report);

like(
	$content,
	qr/^role "r_bad": no_such_setting=on: unrecognized configuration parameter "no_such_setting"$/m,
	'unknown parameter in role setting is reported');
like(
	$content,
	qr/^database "d_bad": standard_conforming_strings=off: /m,
	'rejected value in database setting is reported');
like(
	$content,
	qr/^role "r_bad" in database "d_bad": default_toast_compression=no_such_method: invalid value/m,
	'invalid value in role setting in database is reported');
like(
	$content,
	qr/^function public\.f_bad\(\) in database "postgres": standard_conforming_strings=off: /m,
	'rejected value in function setting is reported');
SKIP:
{
	skip "parameter privileges need PostgreSQL 15 or later", 1
	  unless $have_param_acl;
	like(
		$content,
		qr/^privileges on parameter "no_such_builtin": unrecognized configuration parameter/m,
		'privileges on unknown parameter are reported');
}
unlike($content, qr/r_ok|d_ok|f_ok|work_mem|myext/,
	'settings that restore cleanly are not reported');
unlike($content, qr/no_such_setting_/, 'settings not dumped are not reported');

rmtree($new->data_dir . "/pg_upgrade_output.d");

# Remove the bad settings and upgrade.
$old->start;
$old->safe_psql(
	'postgres', q{
	SET allow_system_table_mods = on;
	DELETE FROM pg_db_role_setting
	  WHERE setrole = 'r_bad'::regrole
	     OR setdatabase = (SELECT oid FROM pg_database WHERE datname = 'd_bad');
	UPDATE pg_proc SET proconfig = NULL WHERE proname = 'f_bad';
});
$old->safe_psql('postgres',
	q{REVOKE SET ON PARAMETER no_such_builtin FROM r_ok})
  if $have_param_acl;
$old->stop;

command_ok(\@pg_upgrade_cmd,
	'pg_upgrade succeeds once the rejected settings are removed');

$new->start;
is( $new->safe_psql(
		'postgres', q{
		SELECT coalesce(r.rolname, '-') || ' ' || coalesce(d.datname, '-') || ' ' ||
		       array_to_string(s.setconfig, ',')
		FROM pg_db_role_setting s
		  LEFT JOIN pg_roles r ON r.oid = s.setrole
		  LEFT JOIN pg_database d ON d.oid = s.setdatabase
		ORDER BY 1}),
	qq{- d_ok default_text_search_config=public.mycfg
r_ok - default_tablespace=no_such_tablespace,myext.foo=bar},
	'role and database settings are kept');
is( $new->safe_psql(
		'postgres',
		q{SELECT array_to_string(proconfig, ',') FROM pg_proc WHERE proname = 'f_ok'}
	),
	'work_mem=2MB,search_path=public, pg_temp',
	'function settings are kept');
if ($have_param_acl)
{
	is( $new->safe_psql(
			'postgres', q{SELECT parname FROM pg_parameter_acl ORDER BY 1}),
		'work_mem',
		'privileges on parameters are kept');
}
$new->stop;

done_testing();
