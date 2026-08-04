
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Data::Dumper;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# ---------------------------------------------------------------------------
# Non-interactive coverage: SHELL_EXIT, \connect reset, --help=variables
# These exercise common.c / command.c / help.c without needing a PTY.
# ---------------------------------------------------------------------------

# SHELL_EXIT after successful SQL, failed SQL, and \!
{
	my ($ret, $stdout, $stderr) = $node->psql(
		'postgres',
		q{SELECT 1;
\echo success=:SHELL_EXIT
SELECT 1/0;
\echo fail=:SHELL_EXIT
\! true
\echo shellok=:SHELL_EXIT
\! false
\echo shellfail=:SHELL_EXIT
},
		on_error_stop => 0);

	like($stdout, qr/^success=0$/m, 'SHELL_EXIT is 0 after successful SQL');
	like($stdout, qr/^fail=1$/m,    'SHELL_EXIT is 1 after failed SQL');
	like($stdout, qr/^shellok=0$/m, 'SHELL_EXIT is 0 after \! true');
	like($stdout, qr/^shellfail=1$/m,
		'SHELL_EXIT is 1 after \! false');
}

# \connect resets ROW_COUNT and SHELL_EXIT
{
	my ($ret, $stdout, $stderr) = $node->psql(
		'postgres',
		q{SELECT 1 AS a, 2 AS b;
\echo before_rc=:ROW_COUNT before_se=:SHELL_EXIT
\connect
\echo after_rc=:ROW_COUNT after_se=:SHELL_EXIT
});

	is($ret, 0, '\connect reset: exit code 0');
	like(
		$stdout,
		qr/^before_rc=1 before_se=0$/m,
		'\connect reset: ROW_COUNT and SHELL_EXIT set before reconnect');
	like(
		$stdout,
		qr/^after_rc=0 after_se=0$/m,
		'\connect reset: ROW_COUNT and SHELL_EXIT cleared after reconnect');
}

# --help=variables documents the new knobs
{
	my ($stdout, $stderr);
	my $result = IPC::Run::run [ 'psql', '--help=variables' ],
	  '>' => \$stdout,
	  '2>' => \$stderr;
	ok($result, 'psql --help=variables exit code 0');
	like($stdout, qr/PROMPT_COMMAND/,
		'--help=variables mentions PROMPT_COMMAND');
	like($stdout, qr/PROMPT_SESSION_EXPORT/,
		'--help=variables mentions PROMPT_SESSION_EXPORT');
	like($stdout, qr/SHELL_EXIT/,
		'--help=variables mentions SHELL_EXIT');
	is($stderr, '', 'psql --help=variables nothing to stderr');
}

# ---------------------------------------------------------------------------
# Interactive coverage: PROMPT_COMMAND, %D, PROMPT_SESSION_EXPORT gating
# Requires IO::Pty (same as t/010_tab_completion.pl / t/030_pager.pl).
# ---------------------------------------------------------------------------

eval { require IO::Pty; };
if ($@)
{
	note 'skipping interactive PROMPT_COMMAND tests: IO::Pty is not available';
	$node->stop;
	done_testing();
	exit;
}

# fire up an interactive psql session
my $h = $node->interactive_psql('postgres');
$h->set_query_timer_restart();

# Helper: send input and assert stdout matches $pattern
sub expect_output
{
	my ($send, $pattern, $annotation) = @_;

	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $out = $h->query_until($pattern, $send);
	my $okay = ($out =~ $pattern && !$h->{timeout}->is_expired);
	ok($okay, $annotation);
	local $Data::Dumper::Terse = 1;
	local $Data::Dumper::Useqq = 1;
	diag 'Actual output was ' . Dumper($out) . "Did not match \"$pattern\"\n"
	  if !$okay;
	return $out;
}

# PROMPT_COMMAND + %D: first line of stdout appears in the prompt
expect_output(
	"\\set PROMPT_COMMAND 'printf PC_OK'\n\\set PROMPT1 '%D# '\n",
	qr/PC_OK# /,
	'PROMPT_COMMAND stdout is substituted via %D in PROMPT1');

# Only the first line of PROMPT_COMMAND stdout is captured
my $first_line_out = expect_output(
	"\\set PROMPT_COMMAND 'echo LINE1; echo LINE2'\n",
	qr/LINE1# /,
	'PROMPT_COMMAND captures only the first line of stdout');
unlike(
	$first_line_out,
	qr/LINE2# /,
	'PROMPT_COMMAND does not embed later stdout lines in the prompt');

# Unset PROMPT_COMMAND leaves %D empty
expect_output(
	"\\unset PROMPT_COMMAND\n\\set PROMPT1 '<%D># '\n",
	qr/<># /,
	'%D is empty when PROMPT_COMMAND is unset');

# PROMPT_SESSION_EXPORT off (default): PSQL_* session vars not exported
# Use PSQL_TXN / PSQL_SUPERUSER — only set by export_prompt_environment().
expect_output(
	"\\set PROMPT_SESSION_EXPORT off\n"
	  . "\\set PROMPT_COMMAND 'printf \"X=%s/%s\" \"\${PSQL_TXN:-none}\" \"\${PSQL_SUPERUSER:-none}\"'\n"
	  . "\\set PROMPT1 '%D# '\n",
	qr/X=none\/none# /,
	'PROMPT_SESSION_EXPORT off: PSQL_TXN/PSQL_SUPERUSER not in subprocess env');

# PROMPT_SESSION_EXPORT on: connection/session state exported to subprocess
expect_output(
	"\\set PROMPT_SESSION_EXPORT on\n"
	  . "\\set PROMPT_COMMAND 'printf \"X=%s/%s\" \"\${PSQL_TXN:-none}\" \"\${PSQL_SUPERUSER:-none}\"'\n",
	qr/X=idle\/[01]# /,
	'PROMPT_SESSION_EXPORT on: PSQL_TXN and PSQL_SUPERUSER exported');

# PROMPT_COMMAND must not overwrite SHELL_EXIT (last *user* command status)
expect_output(
	"SELECT 1/0;\n\\echo se_after_fail=:SHELL_EXIT\n",
	qr/se_after_fail=1/,
	'SHELL_EXIT is 1 after failed SQL (setup for PROMPT_COMMAND isolation)');

# PROMPT_COMMAND that exits non-zero should leave SHELL_EXIT alone
expect_output(
	"\\set PROMPT_COMMAND 'false; printf STILL'\n\\echo se_after_pc=:SHELL_EXIT\n",
	qr/se_after_pc=1/,
	'PROMPT_COMMAND does not update SHELL_EXIT');

# send explicit \q so the pty closes cleanly
$h->quit or die "psql returned $?";

$node->stop;
done_testing();
