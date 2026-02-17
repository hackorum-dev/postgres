#!/usr/bin/perl
#
# Perl script that checks SGML/XSL syntax and formatting issues.
#
# doc/src/sgml/sgml_syntax_check.pl
#
# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use Getopt::Long;

use File::Find;

my $srcdir;
my $builddir;
my $dep_file;
my $stamp_file;

GetOptions(
	'srcdir:s' => \$srcdir,
	'builddir:s' => \$builddir,
	'dep_file:s' => \$dep_file,
	'stamp_file:s' => \$stamp_file) or die "$0: wrong arguments";

die "$0: --srcdir must be specified\n" unless defined $srcdir;

# Find files to process - all the sgml and xsl files (including in subdirectories)
my @files_to_process;
my @dirs_to_search = ($srcdir);
push @dirs_to_search, $builddir if defined $builddir;
find(
	sub {
		return unless -f $_;
		return if $_ !~ /\.(sgml|xsl)$/;
		push @files_to_process, $File::Find::name;
	},
	@dirs_to_search,);

# tabs and non-breaking spaces are harmless, but it is best to avoid them in SGML files
sub check_tabs_and_nbsp
{
	my $errors = 0;
	for my $f (@files_to_process)
	{
		open my $fh, "<:encoding(UTF-8)", $f or die "Can't open $f: $!";
		my $line_no = 0;
		while (<$fh>)
		{
			$line_no++;
			if (/\t/)
			{
				print STDERR "Tab found in $f:$line_no\n";
				$errors++;
			}
			if (/\xC2\xA0/)
			{
				print STDERR "$f:$line_no: contains non-breaking space\n";
				$errors++;
			}
		}
		close($fh);
	}

	if ($errors)
	{
		die "Tabs and/or non-breaking spaces appear in SGML/XML files\n";
	}
}

sub write_dep_file()
{
	open my $fh, '>', $dep_file or die "can't open $dep_file: $!";

	foreach my $file (@files_to_process)
	{
		print $fh "$dep_file : $file\n";
	}

	close $fh;
}

sub create_stamp_file
{
	open my $fh, '>', $stamp_file
	  or die "can't open $stamp_file: $!";
	close $fh;
}

# Create a dependency file for meson build
if ($dep_file)
{
	write_dep_file();
}

check_tabs_and_nbsp();

# All checks are passed, we can create a stamp file meson build now
if ($stamp_file)
{
	create_stamp_file();
}
