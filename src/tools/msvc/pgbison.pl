# -*-perl-*- hey - emacs - this is a perl file

# src/tools/msvc/pgbison.pl

use strict;
use warnings;

use File::Basename;

my $bison = $ENV{BISON_EXE} // 'bison';

# assume we're in the build root.
if (-e 'src/tools/msvc/buildenv.pl') {
	do './src/tools/msvc/buildenv.pl';
} elsif (-e './src/tools/msvc/buildenv_default.pl') {
	do './src/tools/msvc/buildenv_default.pl';
} else {
	die 'Could not find src/tools/msvc/buildenv.pl or ./src/tools/msvc/buildenv_default.pl. Run pgbison from the build root, not src/test/msvc';
}

our $bison;
if (! $bison) {
	$bison = 'bison';
}

my ($bisonver) = `"$bison" -V`;    # grab first line
if ($? ne 0) {
	die "bison (\"$bison\") died with $?, see stderr output above. Check PATH and buildenv.pl"
}
$bisonver = (split(/\s+/, $bisonver))[3];    # grab version number

unless ($bisonver eq '1.875' || $bisonver ge '2.2')
{
	print "WARNING! Bison install not found, or unsupported Bison version.\n";
	print "echo Attempting to build without.\n";
	exit 0;
}

my $input = shift;
if (!defined($input)) {
	print STDERR "Using bison from ${bison}\n";
	die "usage: src/test/msvc/pgbison.pl inputfile.l [...]";
}
if ($input !~ /\.y$/)
{
	print "Input must be a .y file\n";
	exit 1;
}
elsif (!-e $input)
{
	print "Input file $input not found\n";
	exit 1;
}

(my $output = $input) =~ s/\.y$/.c/;

# plpgsql just has to be different
$output =~ s/gram\.c$/pl_gram.c/ if $input =~ /src.pl.plpgsql.src.gram\.y$/;

my $makefile = dirname($input) . "/Makefile";
my ($mf, $make);
open($mf, '<', $makefile);
local $/ = undef;
$make = <$mf>;
close($mf);
my $basetarg = basename($output);
my $headerflag = ($make =~ /^$basetarg:\s+BISONFLAGS\b.*-d/m ? '-d' : '');

my $nodep = $bisonver ge '3.0' ? "-Wno-deprecated" : "";

system("\"$bison\" $nodep $headerflag $input -o $output");
exit $? >> 8;
