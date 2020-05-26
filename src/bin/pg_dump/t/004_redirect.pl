use strict;
use warnings;
use TestLib;
use Test::More tests => 4;

command_fails_like(
	['pg', 'dump'],
	qr/error: connection to database .* failed/,
	'pg pg_dump fails');

command_ok(
	['pg', 'dump', '--version'],
	'pg pg_dump version ok');

command_ok(
	['pg', 'dump', '--help'],
	'pg pg_dump help ok');
