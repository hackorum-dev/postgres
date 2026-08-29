
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test the port implementation of getopt_long().

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

my $longopts = 'alpha,beta:,gamma::,*verbose,alphabet';

# Each case is a paragraph: a "== title" line, optional "optstring:" and
# "args:" lines (default "ab:" and none), then the expected output lines.
# An optional final "stderr: REGEX" line gives the expected error message;
# without it stderr must be empty.  The system getopt_long() words messages
# differently, so the regexes accept both forms.
my $cases = <<'EOF';
== no arguments
--

== only non-options
args: x y
-- x y

== short no arg
args: -a
-a
--

== short attached arg
args: -bfoo
-b=foo
--

== short separate arg
args: -b foo
-b=foo
--

== short bundled
args: -ab foo
-a
-b=foo
--

== short bundled, attached arg
args: -abfoo
-a
-b=foo
--

== short arg looks like option
args: -b -a
-b=-a
--

== short missing arg
args: -b
?
--
stderr: requires an argument -- '?b'?

== short missing arg, silent
optstring: :ab:
args: -b
:
--

== short missing arg at end of bundle
args: -ab
-a
?
--
stderr: requires an argument -- '?b'?

== short unknown
args: -x
?
--
stderr: (illegal|invalid) option -- '?x'?

== short unknown, silent
optstring: :ab:
args: -x
?
--

== short unknown in bundle
args: -axa foo
-a
?
-a
-- foo
stderr: (illegal|invalid) option -- '?x'?

== long no arg
args: --alpha
--alpha
--

== long no arg then non-option
args: --alpha foo
--alpha
-- foo

== long no arg, longer name also defined
args: --alphabet
--alphabet
--

== long unknown
args: --nope
?
--
stderr: illegal option -- nope|unrecognized option [`']--nope'

== long unknown, silent
optstring: :ab:
args: --nope
?
--

== long unknown then option
args: --nope -a
?
-a
--
stderr: illegal option -- nope|unrecognized option [`']--nope'

== long required, equals
args: --beta=foo
--beta=foo
--

== long required, separate
args: --beta foo
--beta=foo
--

== long required, empty
args: --beta=
--beta=
--

== long required, value contains equals
args: --beta=a=b
--beta=a=b
--

== long required, double dash as value
args: --beta --
--beta=--
--

== long required, separate then more
args: --beta foo -a bar
--beta=foo
-a
-- bar

== long required, value looks like option
args: --beta --alpha
--beta=--alpha
--

== long required, missing
args: --beta
?
--
stderr: requires an argument -- beta|[`']--beta' requires an argument

== long required, missing, silent
optstring: :ab:
args: --beta
:
--

== long required, missing after others
args: -a --beta
-a
?
--
stderr: requires an argument -- beta|[`']--beta' requires an argument

== long optional, alone
args: --gamma
--gamma
--

== long optional, alone, silent
optstring: :ab:
args: --gamma
--gamma
--

== long optional, equals
args: --gamma=foo
--gamma=foo
--

== long optional, empty
args: --gamma=
--gamma=
--

== long optional, separate not consumed
args: --gamma foo
--gamma
-- foo

== long optional, separate not consumed, silent
optstring: :ab:
args: --gamma foo
--gamma
-- foo

== long optional, first
args: --gamma -a foo
--gamma
-a
-- foo

== long optional, first with equals
args: --gamma=x -a foo
--gamma=x
-a
-- foo

== long optional, middle
args: -a --gamma -b foo
-a
--gamma
-b=foo
--

== long optional, middle before non-option
args: -a --gamma foo -b bar
-a
--gamma
-b=bar
-- foo

== long optional between non-options
args: foo --gamma bar -a
--gamma
-a
-- foo bar

== long optional, last
args: -a -b foo --gamma
-a
-b=foo
--gamma
--

== long optional, last, silent
optstring: :ab:
args: -a --gamma
-a
--gamma
--

== long optional followed by long
args: --gamma --alpha
--gamma
--alpha
--

== long optional followed by long required
args: --gamma --beta foo
--gamma
--beta=foo
--

== long optional followed by lone dash
args: --gamma -
--gamma
-- -

== long optional repeated
args: --gamma --gamma=1 --gamma
--gamma
--gamma=1
--gamma
--

== long optional then double dash
args: --gamma -- foo
--gamma
-- foo

== long flag
args: --verbose
flag:--verbose
--

== long flag among others
args: -a --verbose --gamma
-a
flag:--verbose
--gamma
--

== double dash ends options
args: -a -- -b foo
-a
-- -b foo

== double dash first
args: -- -a
-- -a

== double dash last
args: -a --
-a
--

== double dash repeated
args: -a -- -- foo
-a
-- -- foo

== non-options reordered to end
args: foo -a bar -b baz qux
-a
-b=baz
-- foo bar qux

== lone dash is a non-option
args: -a - -b x
-a
-b=x
-- -

== non-options then double dash
args: foo -a -- -b bar
-a
-- foo -b bar
EOF

# test_getopt_long_system uses the system getopt_long() where there is one;
# its outputs must match, only the error message wording differs.
foreach my $exe ('test_getopt_long', 'test_getopt_long_system')
{
	foreach my $case (split /\n\n/, $cases)
	{
		my @lines = split /\n/, $case;
		my ($title) = shift(@lines) =~ /^== (.*)/;
		my $optstring = 'ab:';
		$optstring = $1 if $lines[0] =~ /^optstring: (.*)/ and shift @lines;
		my @args;
		@args = split ' ', $1 if $lines[0] =~ /^args: (.*)/ and shift @lines;
		my $stderr_re;
		$stderr_re = $1 if $lines[-1] =~ /^stderr: (.*)/ and pop @lines;

		my ($stdout, $stderr) =
		  run_command([ $exe, $optstring, $longopts, @args ]);
		is($stdout, join("\n", @lines), "$exe $title: output");
		if (defined $stderr_re)
		{
			like($stderr, qr/$stderr_re/, "$exe $title: stderr");
		}
		else
		{
			is($stderr, '', "$exe $title: no stderr");
		}
	}
}

done_testing();
