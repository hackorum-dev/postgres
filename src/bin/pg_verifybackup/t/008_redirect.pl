use strict;
use warnings;
use TestLib;
use Test::More tests => 4;

command_fails_like(
	['pg', 'verifybackup'],
	qr/fatal: no backup directory specified/,
	'pg pg_verifybackup fails');

command_ok(
	['pg', 'verifybackup', '--version'],
	'pg pg_verifybackup version ok');

command_ok(
	['pg', 'verifybackup', '--help'],
	'pg pg_verifybackup help ok');
