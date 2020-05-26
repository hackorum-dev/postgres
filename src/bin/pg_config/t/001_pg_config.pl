use strict;
use warnings;
use TestLib;
use Test::More tests => 23;

program_help_ok('pg_config');
program_version_ok('pg_config');
program_options_handling_ok('pg_config');
command_like([ 'pg_config', '--bindir' ], qr/bin/, 'pg_config single option')
  ;    # XXX might be wrong
command_like([ 'pg_config', '--bindir', '--libdir', '--libexecdir' ],
	qr/bin.*\n.*lib.*\n.*libexec/, 'pg_config three options');
command_like([ 'pg_config', '--libexecdir', '--libdir', '--bindir' ],
	qr/libexec.*\n.*lib.*\n.*bin/, 'pg_config three options different order');
command_like(['pg_config'],
	qr/libexec/, 'pg_config without options includes libexec in the output');
command_like(['pg_config'], qr/.*\n.*\n.*/,
	'pg_config without options prints many lines');
