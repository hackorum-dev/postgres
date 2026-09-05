#!/usr/bin/env perl
use strict;
use warnings;
use utf8;

# Ensure standard filehandles handle UTF-8 properly
binmode STDIN, ":encoding(UTF-8)";
binmode STDOUT, ":encoding(UTF-8)";

my %map;
my %regex_metachars = map { $_ => 1 } split //, '<.*+?|()[]{}^$\\-:=';

# Exact strings that should bypass the transformation
my %denylist = (
	'UTF8' => 1,
	'\x' => 1);

# Helper to map uppercase/lowercase pairs identically across byte boundaries
sub add_pair
{
	my ($u_ascii, $l_ascii, $u_uni, $l_uni) = @_;
	$map{$u_ascii} = chr($u_uni);
	$map{$l_ascii} = chr($l_uni);
}

# 1. Letters mapping (Preserving Postgres casing rules)
# A-C: 1-byte
add_pair('A', 'a', 0x0041, 0x0061);
add_pair('B', 'b', 0x0042, 0x0062);
add_pair('C', 'c', 0x0043, 0x0063);

# D-K: 2-byte (Latin Extended-A)
add_pair('D', 'd', 0x010A, 0x010B);    # Ċ/ċ
add_pair('E', 'e', 0x0112, 0x0113);    # Ē/ē
add_pair('F', 'f', 0x011E, 0x011F);    # Ğ/ğ
add_pair('G', 'g', 0x0122, 0x0123);    # Ģ/ģ
add_pair('H', 'h', 0x0128, 0x0129);    # Ĩ/ĩ
add_pair('I', 'i', 0x012A, 0x012B);    # Ī/ī
add_pair('J', 'j', 0x0132, 0x0133);    # Ĳ/ĳ
add_pair('K', 'k', 0x0136, 0x0137);    # Ķ/ķ

# L-S: 3-byte (Fullwidth Latin)
for my $i (11 .. 18)
{
	add_pair(chr(0x41 + $i), chr(0x61 + $i), 0xFF21 + $i, 0xFF41 + $i);
}

# T-Z: 4-byte (Deseret)
for my $i (19 .. 25)
{
	add_pair(chr(0x41 + $i), chr(0x61 + $i), 0x10400 + $i, 0x10428 + $i);
}

# 2. Digits mapping
$map{'0'} = '0';             # 1-byte
$map{'1'} = chr(0x0661);     # 2-byte (Arabic-Indic)
$map{'2'} = chr(0x0662);
$map{'3'} = chr(0x0663);
$map{'4'} = chr(0xFF14);     # 3-byte (Fullwidth)
$map{'5'} = chr(0xFF15);
$map{'6'} = chr(0xFF16);
$map{'7'} = chr(0x1D7E9);    # 4-byte (Math Sans-Serif)
$map{'8'} = chr(0x1D7EA);
$map{'9'} = chr(0x1D7EB);

# 3. Other ASCII characters
$map{'\\'} = '\\';    # Lock backslash to 1-byte to protect E'\n' escapes
my @symbols;
for my $i (32 .. 126)
{
	my $c = chr($i);
	push @symbols, $c if $c !~ /[A-Za-z0-9']/ && !exists $map{$c};
}

my $base2_sym = 0x00A1;     # ¡ (2-byte)
my $base3_sym = 0x2010;     # ‐ (3-byte)
my $base4_sym = 0x1D300;    # 𝌀 (4-byte)

for my $i (0 .. $#symbols)
{
	my $c = $symbols[$i];
	if ($i < 3)
	{
		$map{$c} = $c;
	}
	elsif ($i < 13)
	{
		$map{$c} = chr($base2_sym++);
	}
	elsif ($i < 23)
	{
		$map{$c} = chr($base3_sym++);
	}
	else
	{
		$map{$c} = chr($base4_sym++);
	}
}

sub transform_string
{
	my ($text, $mode) = @_;

	return $text if $mode eq 'control';
	return $text if $denylist{$text};

	# For format strings, protect PostgreSQL %-escapes
	# (e.g. %s, %I, %L, %% and parametrized ones like %1$s, %-10s)
	if ($mode eq 'format')
	{
		my $res = "";
		# Scans the string chunk by chunk, matching anything up to a format specifier or end of string.
		# The specifier regex captures: %, optional -, numbers, $, *, and the final type letter (s, I, L, or %).
		while ($text =~ /\G(.*?)(%[-0-9\$*]*[sIL%]|$)/gcs)
		{
			my $pre = $1;
			my $spec = $2;

			# Recursively apply 'normal' transformation to the text between format specifiers
			$res .= transform_string($pre, 'normal') if length $pre;
			$res .= $spec if length $spec;
		}
		return $res;
	}

	my $res = "";
	my $is_regex = ($mode eq 'regex');
	my @chars = split //, $text;

	for (my $i = 0; $i < @chars; $i++)
	{
		my $c = $chars[$i];

		# Pass through doubled single quotes untouched
		if ($c eq "'" && $i + 1 < @chars && $chars[ $i + 1 ] eq "'")
		{
			$res .= "''";
			$i++;
			next;
		}

		if ($is_regex)
		{
			# Skip transformation for escaped regex sequences (e.g., \d, \s)
			if ($c eq '\\' && $i + 1 < @chars)
			{
				$res .= $c . $chars[ $i + 1 ];
				$i++;
				next;
			}
			if ($regex_metachars{$c})
			{
				$res .= $c;
				next;
			}
		}

		$res .= $map{$c} // $c;
	}
	return $res;
}

# Slurp the entire input stream
local $/ = undef;
my $sql = <STDIN>;
my $out = "";

# Stack tracks nested function calls: { name => 'regexp_replace', arg => 0 }
my @stack;
my $last_id = "";

pos($sql) = 0;

# The regex evaluates alternatives in order.
# It matches comments and dollar-quotes first, returning them unmodified.
# It only captures and transforms single-quoted strings.
while (
	$sql =~ m/\G(
    (--.*?\n)                           # $2: single-line comment
  | (\/\*.*?\*\/)                       # $3: multi-line comment
  | (\$([a-zA-Z0-9_]*)\$.*?\$\5\$)      # $4: dollar quote, $5: tag
  | (([EeuU]&?)?('(?:[^']|'')*'))       # $6: full string, $7: prefix, $8: content
  | ([a-zA-Z_][a-zA-Z0-9_]*)            # $9: identifier
  | ([(),])                             # $10: punctuation
  | (\s+)                               # $11: whitespace
  | (.)                                 # $12: other
)/gsx)
{
	if (defined $2)
	{
		$out .= $2;
		$last_id = "";
	}
	elsif (defined $3)
	{
		$out .= $3;
		$last_id = "";
	}
	elsif (defined $4)
	{
		$out .= $4;
		$last_id = "";
	}
	elsif (defined $6)
	{
		my $prefix = $7 // "";
		my $literal = $8;
		$literal =~ s/^'//;
		$literal =~ s/'$//;

		my $mode = 'normal';

		# 1. Apply function argument rules based on the stack
		if (@stack)
		{
			my $func = lc($stack[-1]{name});
			my $arg_idx = $stack[-1]{arg};

			# Apply arg index rules for regexp functions
			if ($func =~ /^regexp_/)
			{
				if ($arg_idx == 0)
				{
					$mode = 'normal';
				}
				elsif ($arg_idx == 1)
				{
					$mode = 'regex';
				}
				elsif ($arg_idx == 2 && $func eq 'regexp_replace')
				{
					$mode = 'normal';
				}
				else
				{
					$mode = 'control';
				}
			}
			elsif ($func eq 'encode')
			{
				if ($arg_idx == 0 || $arg_idx == 1)
				{
					$mode = 'control';
				}
			}
			elsif ($func eq 'decode')
			{
				if ($arg_idx == 1)
				{
					$mode = 'control';
				}
			}
			elsif ($func eq 'format')
			{
				if ($arg_idx == 0)
				{
					$mode = 'format';
				}
			}
		}

		# 2. Look ahead in the unparsed text to see if it's cast to bytea or regclass
		my $remainder = substr($sql, pos($sql));
		if ($remainder =~ /^\s*::\s*(?:bytea|regclass)\b/i)
		{
			$mode = 'control';
		}

		$out .= $prefix . "'" . transform_string($literal, $mode) . "'";
		$last_id = "";
	}
	elsif (defined $9)
	{
		$out .= $9;
		# Exclude common SQL syntax keywords from being treated as function names
		if ($9 !~ /^(from|for|as|and|or|not|in)$/i)
		{
			$last_id = $9;
		}
	}
	elsif (defined $10)
	{
		my $p = $10;
		$out .= $p;
		if ($p eq '(')
		{
			push @stack, { name => $last_id, arg => 0 };
		}
		elsif ($p eq ',')
		{
			$stack[-1]{arg}++ if @stack;
		}
		elsif ($p eq ')')
		{
			pop @stack if @stack;
		}
		$last_id = "";
	}
	elsif (defined $11)
	{
		$out .= $11;
	}
	elsif (defined $12)
	{
		$out .= $12;
		$last_id = "";
	}
}

print $out;
