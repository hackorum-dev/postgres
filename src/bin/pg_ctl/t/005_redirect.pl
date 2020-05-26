use strict;
use warnings;
use TestLib;
use Test::More tests => 4;

command_fails_like(
	['pg', 'ctl'],
	qr/pg_ctl: no operation specified/,
	'pg pg_ctl fails');

command_ok(
	['pg', 'ctl', '--version'],
	'pg pg_ctl version ok');

command_ok(
	['pg', 'ctl', '--help'],
	'pg pg_ctl help ok');
