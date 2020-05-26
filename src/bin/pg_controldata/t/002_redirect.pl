use strict;
use warnings;
use TestLib;
use Test::More tests => 4;

command_fails_like(
	['pg', 'controldata'],
	qr/error: no data directory specified/,
	'pg controldata fails');

command_ok(
	['pg', 'controldata', '--version'],
	'pg controldata version ok');

command_ok(
	['pg', 'controldata', '--help'],
	'pg controldata help ok');
