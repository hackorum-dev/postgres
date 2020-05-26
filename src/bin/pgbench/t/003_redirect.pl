use strict;
use warnings;
use TestLib;
use Test::More tests => 4;

command_fails_like(
	['pg', 'bench'],
	qr/error: connection to database .* failed/,
	'pg bench fails');

command_ok(
	['pg', 'bench', '--version'],
	'pg bench version ok');

command_ok(
	['pg', 'bench', '--help'],
	'pg bench help ok');
