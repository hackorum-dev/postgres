
# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Test JSON5 support in the recursive descent JSON parser: comments,
# trailing commas, single-quoted strings and unquoted object keys.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

use File::Temp qw(tempfile);

my $dir = PostgreSQL::Test::Utils::tempdir;

sub run_json5
{
	my ($json, @extra_args) = @_;

	my ($fh, $fname) = tempfile(DIR => $dir);
	print $fh $json;
	close($fh);

	return run_command(
		[ "test_json_parser_json5", @extra_args, $fname ]);
}

# Test that $json succeeds in JSON5 mode, but fails outside of it (unless
# plain_ok is set, for cases which are also valid plain JSON).
sub test_json5_only
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($name, $json, %params) = @_;

	my ($stdout, $stderr) = run_json5($json);
	is($stdout, "SUCCESS!", "$name: succeeds in JSON5 mode");
	is($stderr, "", "$name: no error output in JSON5 mode");

	($stdout, $stderr) = run_json5($json, "-p");
	if ($params{plain_ok})
	{
		is($stdout, "SUCCESS!", "$name: also succeeds as plain JSON");
		is($stderr, "", "$name: no error output as plain JSON");
	}
	else
	{
		unlike($stdout, qr/SUCCESS/, "$name: fails as plain JSON");
		isnt($stderr, "", "$name: error output as plain JSON");
	}
}

sub test_json5_error
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($name, $json, $error) = @_;

	my ($stdout, $stderr) = run_json5($json);
	unlike($stdout, qr/SUCCESS/, "$name: fails in JSON5 mode");
	like($stderr, $error, "$name: correct error output");
}

# Test that the semantic (-s) output for $json, parsed in JSON5 mode,
# matches $expected (which should be valid, canonical JSON).
sub test_json5_semantic
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($name, $json, $expected) = @_;

	my ($stdout, $stderr) = run_json5($json, "-s");
	is($stderr, "", "$name: no error output");

	# normalize whitespace the same way for both sides
	(my $got = $stdout) =~ s/\s+//g;
	(my $want = $expected) =~ s/\s+//g;
	is($got, $want, "$name: correct semantic output");
}

# Comments
test_json5_only("line comment", "// hello\n123");
test_json5_only("line comment at eol", "123 // hello");
test_json5_only("block comment", "/* hello */ 123");
test_json5_only("block comment inside array",
	"[1, /* comment */ 2]");
test_json5_only(
	"multi-line block comment",
	"[1,\n/* a\nmulti\nline\ncomment */\n2]");

test_json5_error("unterminated block comment",
	"/* hello", qr/Unterminated.*comment/);

# Trailing commas
test_json5_only("trailing comma in array", "[1, 2, 3,]");
test_json5_only("trailing comma in object", '{"a": 1, "b": 2,}');
test_json5_only("trailing comma in nested array", "[[1,],[2,],]");

# a lone trailing comma is not enough to make an otherwise-empty
# array or object valid
test_json5_error("array with only a comma", "[,]",
	qr/Expected JSON value, but found ","/);
test_json5_error("object with only a comma", "{,}",
	qr/Expected string or "}", but found ","/);

# Single-quoted strings
test_json5_only("single-quoted string", "'hello'");
test_json5_only("single-quoted string in array", "['hello', 'world']");
test_json5_only(
	"single-quoted string with escaped single quote", "'it\\'s'");
test_json5_only(
	"double-quoted string with escaped single quote inside", '"it\\\'s"');

# Unquoted object keys
test_json5_only("unquoted key", "{foo: 1}");
test_json5_only("unquoted key with underscore", "{_foo_bar: 1}");
test_json5_only(
	"mixed quoted and unquoted keys",
	'{foo: 1, "bar": 2, baz: 3}');

# identifiers are only legal in key position, not as bare values, even in
# JSON5 mode
test_json5_error("bare identifier as value", "[foo]",
	qr/Expected JSON value, but found "foo"/);
test_json5_error("bare identifier as top-level value", "foo",
	qr/Expected JSON value, but found "foo"/);

# semantic (structural) checks
test_json5_semantic("unquoted key round-trip", "{foo: 'bar'}",
	'{"foo": "bar"}');
test_json5_semantic(
	"trailing comma round-trip",
	"[1, 2, 3,]",
	"[1,\n2,\n3]");
test_json5_semantic(
	"comments and trailing commas combined",
	q{
		{
			// this is a comment
			foo: 'bar', /* another comment */
			baz: [1, 2, 3,],
		}
	},
	'{"foo": "bar", "baz": [1, 2, 3]}');

done_testing();
