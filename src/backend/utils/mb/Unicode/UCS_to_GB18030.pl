#! /usr/bin/perl
#
# Copyright (c) 2007-2025, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_GB18030.pl
#

# Generate UTF-8 <--> GB18030 code conversion tables from
# "gb-18030-2000.ucm", a Unicode Character Mapping file (UCM) from ICU,
# obtained from https://github.com/unicode-org/icu-data/blob/d9d3a6ed27bb98a7106763e940258f0be8cd995b/charset/data/ucm/gb-18030-2000.ucm
#
# The lines we care about in the source file look like:
#   <UXXXX> \xYY[\xYY...] |n
# where <UXXXX> is the Unicode code point in hex,
# and the \xYY... is the hex byte sequence for GB18030,
# and n is a flag indicating the type of mapping having
# a single value of 0.
#

use strict;
use warnings FATAL => 'all';

use convutils;

my $this_script = 'src/backend/utils/mb/Unicode/UCS_to_GB18030.pl';

# Read the input

my $in_file = "gb-18030-2000.ucm";
open(my $in, '<', $in_file) || die("cannot open $in_file");

my @mapping;
my $in_charmap = 0;

while (<$in>) {
	chomp;
	# Enter CHARMAP section
	if (/^CHARMAP/) {
		$in_charmap = 1;
		next;
	}
	# Exit CHARMAP section
	if (/^END CHARMAP/) {
		$in_charmap = 0;
		last;
	}
	next unless $in_charmap;
	# Skip comments and empty lines
	next if /^#/ || /^$/;

	# Match lines like: <UXXXX> \xYY[\xYY...] |n, and use only (|0) mappings
	if (/^<U([0-9A-Fa-f]+)>\s+((?:\\x[0-9A-Fa-f]{2})+)\s*\|(\d+)/) {
		my ($u, $c, $flag) = ($1, $2, $3);
		next if ($flag ne '0'); # non-0 flags
		my $ucs = hex($u);
		# Remove \x and concatenate bytes
		my $c_hex = $c;
		$c_hex =~ s/\\x//g;
		my $code = hex($c_hex);
		if ($code >= 0x80 && $ucs >= 0x0080) {
			push @mapping, {
				ucs => $ucs,
				code => $code,
				direction => BOTH,
				f => $in_file,
				l => $.
			};
		}
	}
}
close($in);

print_conversion_tables($this_script, "GB18030", \@mapping);
