# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use Encode   qw(encode decode FB_CROAK LEAVE_SRC);
use JSON::PP qw(decode_json);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('log_encoding');
$node->init(extra => [ '--locale=C', '--encoding=UTF8' ]);
$node->append_conf(
	'postgresql.conf', q(
logging_collector = on
log_destination = 'stderr,csvlog,jsonlog'
log_filename = 'default.log'
log_rotation_age = 0
lc_messages = 'C'
));
$node->start;
is($node->safe_psql('postgres', 'SHOW log_encoding'),
	'off', 'log transcoding is disabled by default');
$node->safe_psql('postgres',
	q(CREATE DATABASE legacy ENCODING 'WIN1251' TEMPLATE template0));
$node->safe_psql('postgres',
	q(CREATE DATABASE raw_bytes ENCODING 'SQL_ASCII' TEMPLATE template0));

my $sql = q(DO $$ BEGIN RAISE WARNING 'log_encoding_test %',
  U&'\041F\0440\0438\0432\0435\0442' USING DETAIL = U&'\0414'; END $$);
$node->safe_psql('legacy', $sql);
$node->stop;
my $expected = "log_encoding_test \x{41f}\x{440}\x{438}\x{432}\x{435}\x{442}";
my $default_log = slurp_file($node->data_dir . '/log/default.log');
my $legacy_message = encode('cp1251', $expected);
like($default_log, qr/\Q$legacy_message\E/,
	'the default log retains the database encoding');

my $prefix = "prefix \x{416}";
$node->append_conf(
	'postgresql.conf', q(
log_filename = 'encoding.log'
log_encoding = utf8
));
$node->append_conf('postgresql.conf',
	encode('UTF-8', "log_line_prefix = '$prefix %% %d: '"));
$node->start;
is($node->safe_psql('postgres', 'SHOW log_encoding'),
	'utf8', 'server accepts UTF8 log encoding');

$node->safe_psql('postgres', $sql);
$node->safe_psql('legacy', $sql);
$node->safe_psql('postgres',
	q(CREATE DATABASE japanese ENCODING 'EUC_JIS_2004' TEMPLATE template0));
# EUC_JIS_2004 0xa4f7 maps to both U+304B and U+309A.
$node->safe_psql(
	'japanese', q(DO $$ BEGIN
  RAISE WARNING 'combined %', convert_from(decode('a4f7', 'hex'), 'EUC_JIS_2004');
  END $$));
$node->safe_psql(
	'legacy', q(DO $$ BEGIN
  RAISE WARNING '%', 'long "' || repeat(U&'\0416', 1500) || '" tail';
  END $$));
$node->safe_psql(
	'raw_bytes', q(DO $$ BEGIN RAISE WARNING 'log_encoding_bytes %',
  convert_from(decode('ff', 'hex'), 'SQL_ASCII'); END $$));

$node->safe_psql(
	'raw_bytes', q(DO $$ BEGIN RAISE WARNING '%',
  repeat('A', 1003) || convert_from(decode('ff', 'hex'), 'SQL_ASCII') || 'END';
  END $$));

$node->psql(
	'legacy',
	q(BEGIN; SELECT 1/0; SELECT U&'\0410'; ROLLBACK;),
	on_error_stop => 0);
$node->connect_fails("user=log_encoding_missing_role",
	'log an authentication failure before selecting a database encoding');
$node->stop;

my $long_message = 'long "' . ("\x{416}" x 1500) . '" tail';
my $combined = "combined \x{304b}\x{309a}";
for my $extension (qw(log csv json))
{
	my $bytes = slurp_file($node->data_dir . "/log/encoding.$extension");
	my $text = eval { decode('UTF-8', $bytes, FB_CROAK | LEAVE_SRC) };
	ok(defined $text, "$extension log consists of valid UTF-8");
	my @matches = $text =~ /\Q$expected\E/g;
	is(scalar @matches, 2, "$extension converts both database encodings");
	like($text, qr/\Q$combined\E/, "$extension retains combined characters");
	like($text, qr/log_encoding_missing_role/,
		"$extension records failed login");
	if ($extension eq 'log')
	{
		like(
			$text,
			qr/\Q$prefix % legacy: WARNING:  $expected\E/,
			'a UTF-8 prefix survives logging from a WIN1251 database');
		like(
			$text,
			qr/\Q$prefix % legacy: DETAIL:  \E\x{414}/,
			'the next prefix and its detail are converted independently');
	}
	if ($extension eq 'json')
	{
		my @records = map { decode_json($_) } split /\n/, $bytes;
		my @messages = map { $_->{message} // '' } @records;
		ok( (scalar grep { $_ eq $long_message } @messages),
			'JSON preserves a long converted field including embedded quotes'
		);
		ok((scalar grep { ($_->{state_code} // '') eq '25P02' } @records),
			'logs an error while the transaction is aborted');
		ok( (scalar grep { $_ eq (('A' x 1003) . '\xffEND') } @messages),
			'escape at the end of a full stack buffer preserves the whole message'
		);
		ok( (scalar grep { $_ eq 'log_encoding_bytes \xff' } @messages),
			'JSON preserves the invalid byte as a literal hexadecimal escape'
		);
	}
	else
	{
		my $long_field = $long_message;
		$long_field =~ s/"/""/g if $extension eq 'csv';
		like($text, qr/\Q$long_field\E/,
			"$extension preserves a long converted field including embedded quotes"
		);
		like(
			$text,
			qr/log_encoding_bytes \\xff/,
			"$extension preserves the invalid byte as a hexadecimal escape");
	}
}

done_testing();
