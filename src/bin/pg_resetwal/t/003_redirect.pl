use strict;
use warnings;
use TestLib;
use Test::More tests => 4;

command_fails_like(
	['pg', 'resetwal'],
	qr/error: no data directory specified/,
	'pg pg_resetwal fails');

command_ok(
	['pg', 'resetwal', '--version'],
	'pg pg_resetwal version ok');

command_ok(
	['pg', 'resetwal', '--help'],
	'pg pg_resetwal help ok');
