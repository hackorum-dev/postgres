use strict;
use warnings;
use TestLib;
use Test::More tests => 4;

command_fails_like(
	['pg', 'archivecleanup'],
	qr/error: must specify archive location/,
	'pg pg_archivecleanup fails');

command_ok(
	['pg', 'archivecleanup', '--version'],
	'pg pg_archivecleanup version ok');

command_ok(
	['pg', 'archivecleanup', '--help'],
	'pg pg_archivecleanup help ok');
