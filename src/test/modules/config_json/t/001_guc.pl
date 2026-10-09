# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use File::Copy;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('json');
$node->init;
my $datadir = $node->data_dir;
my $root = "$datadir/postgresql.json";

sub write_file
{
	my ($path, $content) = @_;
	open my $fh, '>', $path or die "cannot write $path: $!";
	print $fh $content;
	close $fh;
}

# Errors the loader found, as pg_file_settings reports them.
my $errors_query = q{SELECT string_agg(error, E'\n') FROM pg_file_settings
	WHERE error IS NOT NULL};

sub file_errors
{
	return $node->safe_psql('postgres', $errors_query);
}

# The JSON root pulls in the postgresql.conf that initdb and Cluster.pm
# wrote (port, sockets, fsync), then adds its own settings.
write_file($root, <<'EOF');
{
  "include": ["postgresql.conf", {"file": "conf.d/*.json", "optional": true}, {"file": "conf.d/*.conf", "optional": true}],
  "work_mem": "7MB",
  "enable_seqscan": false,
  "max_stack_depth": 1500,
  "search_path": ["a", "b"],
  "hba": {"children": [{"import": "pg_hba.conf"}]}
}
EOF

$node->start(options => "-c config_file=$root");

is($node->safe_psql('postgres', 'SHOW work_mem'), '7MB', 'string value');
is($node->safe_psql('postgres', 'SHOW enable_seqscan'),
	'off', 'boolean value');
is($node->safe_psql('postgres', 'SHOW max_stack_depth'),
	'1500kB', 'number value');
is($node->safe_psql('postgres', 'SHOW search_path'),
	'a, b', 'array value joined with commas');

my $res = $node->safe_psql(
	'postgres',
	q{SELECT regexp_replace(sourcefile, '.*/', ''), sourceline, applied
	  FROM pg_file_settings WHERE name = 'work_mem'});
is($res, 'postgresql.json|3|t', 'pg_file_settings provenance for JSON');

# include overrides includer, later include overrides earlier
mkdir "$datadir/conf.d";
write_file("$datadir/conf.d/10-a.json", '{"work_mem": "8MB"}');
write_file("$datadir/conf.d/20-b.json", '{"work_mem": "9MB"}');
$node->reload;
$node->poll_query_until('postgres',
	q{SELECT current_setting('work_mem') = '9MB'})
  or die 'reload did not apply conf.d';
pass('later include wins');

# .conf include through the legacy parser
write_file("$datadir/conf.d/30-c.conf", "work_mem = '10MB'\n");
$node->reload;
$node->poll_query_until('postgres',
	q{SELECT current_setting('work_mem') = '10MB'})
  or die 'legacy include not applied';
pass('.conf include via legacy parser');

# List parameters: quoting where the parameter expects it, plain
# otherwise.
write_file("$datadir/conf.d/40-d.json",
	'{"search_path": ["My Schema", "public"], '
	  . '"log_connections": ["receipt", "authentication"]}');
$node->reload;
$node->poll_query_until('postgres',
	q{SELECT current_setting('search_path') = '"My Schema", public'})
  or die 'quoted list not applied';
pass('GUC_LIST_QUOTE elements quoted');
is($node->safe_psql('postgres', 'SHOW log_connections'),
	'receipt, authentication', 'plain list stays plain');
unlink "$datadir/conf.d/40-d.json";

# A bad setting is only a settings error.  It does not keep the rest of
# the configuration from loading, so new connections keep working even
# where they read the files again (EXEC_BACKEND), as file_errors() shows.
write_file("$datadir/conf.d/41-e.json",
	'{"log_connections": ["receipt,authentication"]}');
like(
	file_errors(),
	qr/elements of "log_connections" must not contain commas \(node \$\.log_connections\[0\], line 1 of file/,
	'comma in plain list element rejected');
unlink "$datadir/conf.d/41-e.json";

# A file that cannot be read as a whole is a different matter: with
# EXEC_BACKEND, new backends may fail to start while it is in place.
# Such checks run in a session opened beforehand.
my $session = $node->background_psql('postgres');

# duplicate key: error in view, reload keeps old values
write_file("$datadir/conf.d/50-f.json",
	'{"work_mem": "1MB", "work_mem": "2MB"}');
like(
	$session->query_safe($errors_query),
	qr/duplicate key "work_mem" \(node \$\.work_mem, line 1 of file ".*50-f\.json"\)/,
	'duplicate key reported with file and line');
$node->reload;
$node->wait_for_log(qr/contains errors; no changes were applied/);
is($session->query_safe('SHOW work_mem'), '10MB',
	'bad reload keeps old value');
unlink "$datadir/conf.d/50-f.json";

# Parameter names are case-insensitive, so this is a duplicate too.
write_file("$datadir/conf.d/51-f.json",
	'{"work_mem": "1MB", "Work_Mem": "2MB"}');
like(
	file_errors(),
	qr/duplicate key "Work_Mem" \(node \$\.Work_Mem, line 1 of file/,
	'duplicate key differing in case reported');
unlink "$datadir/conf.d/51-f.json";

# nested object under a GUC key; a key that is not an identifier is
# written in brackets in the path
write_file("$datadir/conf.d/60-g.json",
	'{"work_mem": {"x": 1}, "pg_stat_statements.max": [{}]}');
$res = file_errors();
like(
	$res,
	qr/"work_mem" must be a string, number, boolean, or array of these \(node \$\.work_mem,/,
	'nested object rejected with path');
like(
	$res,
	qr/\(node \$\["pg_stat_statements\.max"\]\[0\],/,
	'key with a dot in brackets');
unlink "$datadir/conf.d/60-g.json";

# The file-naming parameters are rejected with a JSON root, in the JSON
# documents, whatever the spelling, and in included legacy files.
foreach my $guc (qw(hba_file ident_file hosts_file HBA_File))
{
	write_file("$datadir/conf.d/70-h.json", qq({"$guc": "some.conf"}));
	like(
		file_errors(),
		qr/parameter "$guc" cannot be set with a JSON configuration file \(node \$\.$guc,/,
		"$guc rejected");
}
unlink "$datadir/conf.d/70-h.json";
write_file("$datadir/conf.d/71-h.conf", "hba_file = 'some.conf'\n");
like(
	file_errors(),
	qr/parameter "hba_file" cannot be set with a JSON configuration file/,
	'hba_file rejected in an included legacy file');
unlink "$datadir/conf.d/71-h.conf";

# ALTER SYSTEM would write it to postgresql.auto.conf, which is read
# after the JSON root.
my ($ret, $stdout, $stderr) =
  $node->psql('postgres', "ALTER SYSTEM SET hba_file = 'some.conf'");
like(
	$stderr,
	qr/parameter "hba_file" cannot be set with a JSON configuration file/,
	'ALTER SYSTEM SET hba_file rejected');

# A wildcard that matches the including file itself is direct recursion,
# not a nesting depth problem.
write_file("$datadir/conf.d/80-i.json", '{"include": "*.json"}');
$res = $session->query_safe($errors_query);
like($res, qr/configuration file recursion/, 'self-include rejected');
unlike($res, qr/nesting depth/, 'self-include is not a depth error');
unlink "$datadir/conf.d/80-i.json";

# \u escapes beyond ASCII would depend on the parsing process's encoding
write_file("$datadir/conf.d/81-j.json", '{"work_mem": "7\\u00e9MB"}');
like(
	$session->query_safe($errors_query),
	qr/Unicode escape sequences above U\+007F are not supported \(line 1 of file ".*81-j\.json"\)/,
	'non-ASCII \\u escape rejected');
unlink "$datadir/conf.d/81-j.json";

# Include entry forms: '?' wildcard, wildcard in a directory component,
# non-boolean "optional", unrecognized key.
mkdir "$datadir/extra";
write_file("$datadir/extra/01-x.json", '{"work_mem": "11MB"}');
write_file(
	"$datadir/conf.d/82-k.json",
	'{"include": ["../extra/0?-x.json", "../ex*/01-x.json", '
	  . '{"file": "x.json", "optional": "yes"}, '
	  . '{"file": "x.json", "foo": 1}]}');
is( $session->query_safe(
		q{SELECT count(*) FROM pg_file_settings
		  WHERE sourcefile LIKE '%/extra/01-x.json'}),
	'1',
	'? wildcard matches');
$res = $session->query_safe($errors_query);
like(
	$res,
	qr/wildcards are only allowed in the file name part: "\.\.\/ex\*\/01-x\.json" \(node \$\.include\[1\],/,
	'wildcard in directory component rejected');
like(
	$res,
	qr/"optional" must be a boolean \(node \$\.include\[2\]\.optional,/,
	'non-boolean optional rejected');
like(
	$res,
	qr/unrecognized key "foo" \(node \$\.include\[3\]\.foo,/,
	'unrecognized include key rejected');
unlink "$datadir/conf.d/82-k.json";

# A wildcard that matches nothing is an error unless optional.
write_file("$datadir/conf.d/83-l.json",
	'{"include": ["*.nothing", {"file": "*.none", "optional": true}]}');
$res = $session->query_safe($errors_query);
like(
	$res,
	qr/include pattern "\*\.nothing" matches no files \(node \$\.include\[0\],/,
	'unmatched pattern rejected');
unlike($res, qr/\*\.none/, 'unmatched optional pattern accepted');
unlink "$datadir/conf.d/83-l.json";

$session->quit;
$node->stop;

# Root discovery: postgresql.json next to a postgresql.conf without an
# explicit config_file is ambiguous.
$ret = $node->start(fail_ok => 1);
is($ret, 0, 'both roots present fails');
ok( $node->log_contains(
		qr/both "postgresql.json" and "postgresql.conf" exist/),
	'ambiguity reported');

# Rename the legacy root away: the JSON root is found by itself.
rename("$datadir/postgresql.conf", "$datadir/cluster.conf") or die $!;
my $json = slurp_file($root);
$json =~ s/"postgresql\.conf"/"cluster.conf"/;
write_file($root, $json);
$node->start;
is($node->safe_psql('postgres', 'SHOW config_file'),
	$root, 'postgresql.json discovered as root');
$node->stop;

# hba_file on the command line is rejected as well
$ret = $node->start(fail_ok => 1, options => '-c hba_file=some.conf');
is($ret, 0, 'hba_file on the command line fails');
ok( $node->log_contains(
		qr/parameter "hba_file" cannot be set with a JSON configuration file/),
	'hba_file on the command line reported');

# pg_ctl recognizes a configuration-only directory by postgresql.json, and
# asks the server for the data directory.
my $confdir = PostgreSQL::Test::Utils::tempdir;
my $jdatadir = $datadir =~ s/\\/\\\\/gr;
write_file("$confdir/postgresql.json",
	qq({"data_directory": "$jdatadir",
  "include": ["$jdatadir/cluster.conf"],
  "hba": {"children": [{"import": "$jdatadir/pg_hba.conf"}]}}\n));
copy("$datadir/pg_hba.conf", $confdir) or die $!;
command_ok(
	[
		'pg_ctl', '--wait',
		'--pgdata' => $confdir,
		'--log' => $node->logfile,
		'start'
	],
	'pg_ctl starts from a directory with only postgresql.json');
is($node->safe_psql('postgres', 'SHOW data_directory'),
	$datadir, 'data directory found through postgresql.json');
command_ok([ 'pg_ctl', '--wait', '--pgdata' => $confdir, 'stop' ],
	'pg_ctl stops through the configuration directory');

# An invalid value in the root is fatal at startup, unlike at reload.
write_file($root, '{"include": ["cluster.conf"], "work_mem": "lots"}');
$ret = $node->start(fail_ok => 1);
is($ret, 0, 'bad startup fails');
ok($node->log_contains(qr/contains errors/), 'startup errors reported');

# The good root starts again; conf.d/30-c.conf still overrides it.
write_file($root, $json);
$node->start;
is($node->safe_psql('postgres', 'SHOW work_mem'),
	'10MB', 'restored root works');
$node->stop;

done_testing();
