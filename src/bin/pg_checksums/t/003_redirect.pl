use strict;
use warnings;
use TestLib;
use Test::More tests => 4;

command_fails_like(
	['pg', 'checksums'],
	qr/error: no data directory specified/,
	'pg pg_checksums fails');

command_ok(
	['pg', 'checksums', '--version'],
	'pg pg_checksums version ok');

command_ok(
	['pg', 'checksums', '--help'],
	'pg pg_checksums help ok');
