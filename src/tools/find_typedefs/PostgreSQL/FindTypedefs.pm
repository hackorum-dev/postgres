############################################################################
#
# PostgreSQL/FindTypedefs.pm
#
# Module providing a function to find typedefs
#
# Copyright (c) 2021-2024, PostgreSQL Global Development Group
#
############################################################################

package PostgreSQL::FindTypedefs;

use strict;
use warnings FATAL => 'all';

use Exporter qw(import);
our @EXPORT = qw(typedefs);

use Config;
use File::Find;
use Scalar::Util qw(reftype);

# Returns a reference to a sorted array of typedef names
#
# Arguments are given as a hash. recognized names are:
#    binloc - where to find binary files. Required.
#    srcdir - where to find source files. Required.
#    msvc - boolean for whether we are using MSVC. Optional, default false.
#    hostopt - --host= setting if we are cross-compiling. Optional, default "".
#
# If binloc is given as an arrayref instead of as a scalar, it is taken
# as a list of binary files to be examined rather than as a path to be
# explored using File::Find / glob().
#
# If binloc is a scalar, then on MacOs it's the path to the root of the
# build directory, where we will look at the .o files. Everywhere else it
# needs to be the root of an installation, with bin and lib subdirectories,
# where we will examine built executables and library files.
#
sub typedefs
{
	my %args = @_;

	my $binloc = $args{binloc} || die "no binloc specified";
	my $srcdir = $args{srcdir} || die "no srcdir specified";
	my $using_msvc = $args{msvc} || 0;
	my $hostopt = $args{hostopt} || "";

	# work around the fact that ucrt/binutils objdump is far slower
	# than the one in msys/binutils
	local $ENV{PATH} = $ENV{PATH};
	$ENV{PATH} = "/usr/bin:$ENV{PATH}" if $Config{osname} eq 'msys';

	my $hostobjdump = $hostopt ? "$hostopt-objdump" : "";
	my $objdump = 'objdump';
	my $sep = $using_msvc ? ';' : ':';

	# if we have a hostobjdump, find out which of it and objdump is in the path
	foreach my $p (split(/$sep/, $ENV{PATH}))
	{
		last unless $hostobjdump;
		last if (-e "$p/objdump" || -e "$p/objdump.exe");
		if (-e "$p/$hostobjdump" || -e "$p/$hostobjdump.exe")
		{
			$objdump = $hostobjdump;
			last;
		}
	}
	my @err = `$objdump -W 2>&1`;
	my @readelferr = `readelf -w 2>&1`;
	my $using_osx = (`uname` eq "Darwin\n");
	my @testfiles;
	my %syms;
	my @dumpout;
	my @flds;

	if ((reftype($binloc) || "") eq 'ARRAY')
	{
		@testfiles = @$binloc;
	}
	elsif ($using_osx)
	{
		# On OS X, we need to examine the .o files
		# exclude ecpg/test, which pgindent does too
		my $obj_wanted = sub {
			/^.*\.o\z/s
			  && !($File::Find::name =~ m!/ecpg/test/!s)
			  && push(@testfiles, $File::Find::name);
		};

		File::Find::find($obj_wanted, $binloc);
	}
	else
	{
		# Elsewhere, look at the installed executables and shared libraries
		@testfiles = (
			glob("$binloc/bin/*"),
			glob("$binloc/lib/*"),
			glob("$binloc/lib/postgresql/*")
		);
	}

	foreach my $bin (@testfiles)
	{
		next if $bin =~ m!bin/(ipcclean|pltcl_)!;
		next unless -f $bin;
		next if -l $bin;                        # ignore symlinks to plain files
		next if $bin =~ m!/postmaster.exe$!;    # sometimes a copy not a link

		if ($using_osx)
		{
			@dumpout = `dwarfdump $bin 2>/dev/null`;
			@dumpout = _dump_filter(\@dumpout, 'TAG_typedef', 2);
			foreach (@dumpout)
			{
				## no critic (RegularExpressions::ProhibitCaptureWithoutTest)
				@flds = split;
				if (@flds == 3)
				{
					# old format
					next unless ($flds[0] eq "AT_name(");
					next unless ($flds[1] =~ m/^"(.*)"$/);
					$syms{$1} = 1;
				}
				elsif (@flds == 2)
				{
					# new format
					next unless ($flds[0] eq "DW_AT_name");
					next unless ($flds[1] =~ m/^\("(.*)"\)$/);
					$syms{$1} = 1;
				}
			}
		}
		elsif (@err == 1)    # Linux and sometimes windows
		{
			my $cmd = "$objdump -Wi $bin 2>/dev/null";
			@dumpout = `$cmd`;
			@dumpout = _dump_filter(\@dumpout, 'DW_TAG_typedef', 3);
			foreach (@dumpout)
			{
				@flds = split;
				next unless (1 < @flds);
				next
				  if (($flds[0] ne 'DW_AT_name' && $flds[1] ne 'DW_AT_name')
					|| $flds[-1] =~ /^DW_FORM_str/);
				$syms{ $flds[-1] } = 1;
			}
		}
		elsif (@readelferr > 10)
		{

			# FreeBSD, similar output to Linux
			my $cmd = "readelf -w $bin 2>/dev/null";
			@dumpout = ` $cmd`;
			@dumpout = _dump_filter(\@dumpout, 'DW_TAG_typedef', 3);

			foreach (@dumpout)
			{
				@flds = split;
				next unless (1 < @flds);
				next if ($flds[0] ne 'DW_AT_name');
				$syms{ $flds[-1] } = 1;
			}
		}
		else
		{
			@dumpout = `$objdump --stabs $bin 2>/dev/null`;
			foreach (@dumpout)
			{
				@flds = split;
				next if (@flds < 7);
				next if ($flds[1] ne 'LSYM' || $flds[6] !~ /([^:]+):t/);
				## no critic (RegularExpressions::ProhibitCaptureWithoutTest)
				$syms{$1} = 1;
			}
		}
	}
	my @badsyms = grep { /\s/ } keys %syms;
	push(@badsyms, 'date', 'interval', 'timestamp', 'ANY');
	delete @syms{@badsyms};

	my @goodsyms = sort keys %syms;
	my $foundsyms = [];

	my %foundwords;

	my $setfound = sub {

		# $_ is the name of the file being examined
		# its directory is our current cwd

		return unless (-f $_ && /^.*\.[chly]\z/);

		open(my $srcfile, '<', $_) || die "opening $_: $!";
		local $/ = undef;
		my $src = <$srcfile>;
		close($srcfile);

		# strip C comments
		# Use a simple pattern rather than the recipe in perlfaq6.
		# We don't need to keep the quoted string values anyway, and
		# on some platforms the complex regex causes perl to barf and crash.
		$src =~ s{/\*.*?\*/}{}gs;

		foreach my $word (split(/\W+/, $src))
		{
			$foundwords{$word} = 1;
		}
	};

	File::Find::find($setfound, $srcdir);

	foreach my $sym (@goodsyms)
	{
		push(@$foundsyms, $sym) if exists $foundwords{$sym};
	}

	return $foundsyms;
}

# private routine, poor man's egrep -A
sub _dump_filter
{
	my ($lines, $tag, $context) = @_;
	my @output;
	while (@$lines)
	{
		my $line = shift @$lines;
		if (index($line, $tag) > -1)
		{
			push(@output, splice(@$lines, 0, $context));
		}
	}
	return @output;
}

1;

