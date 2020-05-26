use strict;
use warnings;
use TestLib;
use Test::More tests => 4;

command_fails_like(
	['pg', 'basebackup'],
	qr/error: no target directory specified/,
	'pg pg_basebackup fails');

command_ok(
	['pg', 'basebackup', '--version'],
	'pg pg_basebackup version ok');

command_ok(
	['pg', 'basebackup', '--help'],
	'pg pg_basebackup help ok');
