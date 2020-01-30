# Basic logical replication test
use strict;
use warnings;
use TestLib qw(command_like $windows_os);

use Test::More tests => 83;

my $exe = $windows_os ? ".exe" : "";

# We don't install test_json, so we pick it up from the
# place it  gets built, $ENV{TESTDIR} except for MSVC

my $binloc = ".";
my $using_msvc = 0;

if (-e "../../../test_json.vcxproj") # MSVC
{
	$using_msvc = 1;
	if (-d "../../../Debug/test_json")
	{
		$binloc = "../../../Debug/test_json";
	}
	else
	{
		$binloc = "../../../Release/test_json";
	}
}
elsif (exists $ENV{TESTDIR})
{
	$binloc = $ENV{TESTDIR};
}

my $test_json = "$binloc/test_json$exe";

ok(-f $test_json, "test_json file exists");
ok(-x $test_json, "test_json file is executable");

# Verify some valid JSON is accepted by our parser
command_like( [$test_json, q/null/       ], qr{\bVALID\b}, "null");
command_like( [$test_json, q/{}/         ], qr{\bVALID\b}, "empty object");
command_like( [$test_json, q/[]/         ], qr{\bVALID\b}, "empty array");
command_like( [$test_json, q/-12345/     ], qr{\bVALID\b}, "negative integer");
command_like( [$test_json, q/-1/         ], qr{\bVALID\b}, "negative integer");
command_like( [$test_json, q/0/          ], qr{\bVALID\b}, "zero");
command_like( [$test_json, q/1/          ], qr{\bVALID\b}, "positive integer");
command_like( [$test_json, q/12345/      ], qr{\bVALID\b}, "positive integer");
command_like( [$test_json, q/-1.23456789/], qr{\bVALID\b}, "negative float");
command_like( [$test_json, q/1.23456789/ ], qr{\bVALID\b}, "positive float");
command_like( [$test_json, q/{"a": "b"}/ ], qr{\bVALID\b}, "object");
command_like( [$test_json, q/["a", "b"]/ ], qr{\bVALID\b}, "array");
SKIP:
{
	skip "text string test confuses processor on MSVC", 3 if $using_msvc;

	command_like( [$test_json, q/"pigs feet"/], qr{\bVALID\b}, 'text string');
}

# Verify some invalid JSON is rejected by our parser
command_like( [$test_json, q/{/          ], qr{^\s*The input string ended unexpectedly\.\s*$}ms, 'unclosed object');
command_like( [$test_json, q/[/          ], qr{^\s*The input string ended unexpectedly\.\s*$}ms, 'unclosed array');
command_like( [$test_json, q/(/          ], qr{^\s*Token "\(" is invalid\.\s*$}ms, 'unclosed parenthesis');
command_like( [$test_json, q/}/          ], qr{^\s*Expected JSON value, but found "\}"\.\s*$}ms, 'unopened object');
command_like( [$test_json, q/]/          ], qr{^\s*Expected JSON value, but found "\]"\.\s*$}ms, 'unopened array');
command_like( [$test_json, q/)/          ], qr{^\s*Token "\)" is invalid\.\s*$}ms, 'unopened parenthesis');
command_like( [$test_json, q/{{{}}/      ], qr{^\s*Expected string or "\}", but found "\{"\.\s*$}ms, 'unbalanced object curlies');
command_like( [$test_json, q/{{}}}/      ], qr{^\s*Expected string or "\}", but found "\{"\.\s*$}ms, 'unbalanced object curlies');
command_like( [$test_json, q/[[[]]/      ], qr{^\s*The input string ended unexpectedly\.\s*$}ms, 'unbalanced array braces');
command_like( [$test_json, q/[[]]]/      ], qr{^\s*Expected end of input, but found "\]"\.\s*$}ms, 'unbalanced array braces');
command_like( [$test_json, q/((())/      ], qr{^\s*Token "\(" is invalid\.\s*$}ms, 'unbalanced array braces');
command_like( [$test_json, q/(()))/      ], qr{^\s*Token "\(" is invalid\.\s*$}ms, 'unbalanced array braces');
command_like( [$test_json, q/1 7 13/     ], qr{^\s*Expected end of input, but found "7"\.\s*$}ms, 'integer sequence');
command_like( [$test_json, q/{"a", "b"}/ ], qr{^\s*Expected ":", but found ","\.\s*$}ms, 'mixed object and array syntax');
