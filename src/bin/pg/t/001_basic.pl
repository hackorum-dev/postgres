use strict;
use warnings;

use TestLib;
use Test::More tests => 13;

program_help_ok('pg');
program_version_ok('pg');
program_options_handling_ok('pg');

command_fails_like(
	[ 'pg', '-a' ],
	qr/\Qpg: error: invalid option: -a\E/,
	'pg: invalid command-line arguments');

command_ok(
	['pg', '--version'],
	'pg version ok');

command_ok(
	['pg', '--help'],
	'pg help ok');

# Checks of various commands, such as 'pg initdb', are implemented
# in tests named /.*_redirect.pl/ in the test directory of the
# command in question, so we do not need to duplicate that here.
# But to help developers who change pg.c and run 'make check' in
# the pg directory, it helps to have at least one example of that
# for smoke testing.

command_ok([ 'pg', 'initdb', '--version' ], 'pg redirection');
