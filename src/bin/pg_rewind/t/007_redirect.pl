use strict;
use warnings;
use TestLib;
use Test::More tests => 4;

command_fails_like(
	['pg', 'rewind'],
	qr/error: no source specified/,
	'pg pg_rewind fails');

command_ok(
	['pg', 'rewind', '--version'],
	'pg pg_rewind version ok');

command_ok(
	['pg', 'rewind', '--help'],
	'pg pg_rewind help ok');
