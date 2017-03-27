#! /usr/bin/perl
use strict;

# collect all radix mapfiles
opendir(my $dh, ".") || die "failed to open directory: .";
my @maps = grep { /\.map$/ } readdir($dh);
closedir($dh);

# generate sanity checker source
my $out;
open($out, '>', "map_dumper.h")
  || die "cannot open file to write: map_dumper.h";

# add #include lines for all radix maps and corresponding plain maps
foreach my $i (sort @maps)
{
	print $out "#include \"$i\"\n";
}

print $out <<'EOF';

struct mappair
{
	const char			   *name;
	const pg_mb_radix_tree *rt;
} mappairs[] = {
EOF

# generate variable names for the array of mappair
my @mapnames = map { my $m = $_; $m =~ s/\.map//; $m } @maps;

# write the content of mappairs array.
foreach my $m (@mapnames)
{
	if ($m =~ /^utf8_to_(.*)$/)
	{
		my $e = uc($1);
		print $out
		"	{\"$m\", &$1_from_unicode_tree}";
	}
	elsif ($m =~ /^(.*)_to_utf8$/)
	{
		my $e = uc($1);
		print $out
		  "	{\"$m\", &$1_to_unicode_tree}";
	}
	else
	{
		die "Unrecognizable map name: $m";
	}
	print $out ",\n";
}

print $out "	{NULL, NULL}\n};\n";

close($out);
