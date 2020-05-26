use strict;
use warnings;
use TestLib;
use Test::More tests => 4;

command_fails_like(
	['pg', 'initdb'],
	qr/error: no data directory specified/,
	'pg initdb fails');

command_ok(
	['pg', 'initdb', '--version'],
	'pg initdb version ok');

command_ok(
	['pg', 'initdb', '--help'],
	'pg initdb help ok');
