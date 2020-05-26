use strict;
use warnings;
use TestLib;
use Test::More tests => 4;

command_fails_like(
	['pg', 'waldump'],
	qr/error: no arguments specified/,
	'pg pg_waldump fails');

command_ok(
	['pg', 'waldump', '--version'],
	'pg pg_waldump version ok');

command_ok(
	['pg', 'waldump', '--help'],
	'pg pg_waldump help ok');
