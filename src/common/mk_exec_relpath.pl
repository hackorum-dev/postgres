#!/usr/bin/perl

use strict;
use warnings;

# Some tools are located in the installation's bindir, but execute
# commands located in the libexec directory.  Postgres's configure
# script allows the user to override the defaults for bindir and libexecdir,
# so we cannot assume the default that $(bindir)/../libexec is the libexecdir.
# Nor can we assume that the $(libexec) dir from `make' will be the installed
# path, as our commands are relocatable, and in any event they get installed
# into a different directory for regression testing.
#
# We make the hopefully reasonable assumption that the relative path delta
# between bindir and libexecdir is invariant, compute that here, and write it
# to stdout.  The make system takes it from there.
#
# We are passed bindir and libexecdir on the command line.  For example,
# C:\our\pg\foo\bin and C:\our\pg\some\other\libexecdir.  We generate a
# relative path that can be used to cd from bindir into libexecdir.
# For example, "../../some/other/libexecdir".  Note that we normalize to
# unix style forward slashes.  We also generate the relative path going the
# other way, from libexecdir back to bindir.
#
# TODO: What to do if the paths are windows style, and they are on differing
# mounts?  For example, "C:\pg\bin" vs "D:\pg\libexec"
#

# Given two paths (or path suffixes) rooted at the same location, make a
# relative path from the first to the second.
sub make_relpath($$)
{
	my ($src, $dst) = @_;

	# Replace path elements of src into '..', for example we would
	# convert "foo/bin" into "../.."
	$src =~ s{[^/]+}{..}g;

	# Strip leading slash, if any, from both
	$src =~ s{^/}{};
	$dst =~ s{^/}{};

	return join('/', $src, $dst);
}

my $bin = $ARGV[0];
my $lib = $ARGV[1];

# Create a separator that does not exist in either path argument.  We don't
# need to be clever here.  We just keep appending spaces until we have
# something suitable.
my $sep = ' ';
$sep .= ' ' while ($bin =~ m/$sep/ or $lib =~ m/$sep/);

# Use a regex to find the longest common prefix of the two directories.  For
# example, "C:\our\pg\".  We insist that the common prefix ends with a slash
# character, such as "/" on unix or "C:\" on windows.
#
if ("${bin}${sep}${lib}" =~ m"^(.*[\\/])(.*?)${sep}\1(.*)$")
{
	# Record the suffixes of the two paths starting from where they diverge,
	# for example, "foo\bin" and "some\other\libexecdir"
	my ($binsuffix, $libsuffix) = ($2, $3);

	# Replace windows specific directory separators in both suffixes to get,
	# for example, "foo/bin" and "some/other/libexecdir"
	$binsuffix =~ s{\\}{/}g;
	$libsuffix =~ s{\\}{/}g;

	# Print the two relative paths
	printf("#ifndef PG_EXEC_RELPATH_H\n");
	printf("#define PG_EXEC_RELPATH_H\n\n");
	printf("#define LIBEXEC_SUFFIX \"%s\"\n", $libsuffix);
	printf("#define BIN_SUFFIX \"%s\"\n", $binsuffix);
	printf("#define TO_LIBEXEC_RELPATH \"/%s/\"\n",
		make_relpath($binsuffix, $libsuffix));
	printf("#define TO_BIN_RELPATH \"/%s/\"\n\n",
		make_relpath($libsuffix, $binsuffix));
	printf("#endif			/* PG_EXEC_RELPATH_H */\n");

	# Successful return
	exit 0;
}
warn "Cannot construct relative path for changing directories from $bin to $lib";
exit 1;
