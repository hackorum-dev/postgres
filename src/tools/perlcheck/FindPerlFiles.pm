
# Copyright (c) 2023, PostgreSQL Global Development Group

=pod

=head1 NAME

FindPerlFiles - module for finding perl files from a list of paths

=head1 SYNOPSIS

  use FindPerlFiles;

  my @files = FindPerlFiles::findperl(path, ...);


=head1 DESCRIPTION

FindPerlFiles finds files which either have a perl extension (.pl or .pm) or
are bot executable and found by the `file` program to be perl scripts.

=cut


package FindPerlFiles;

use strict;
use warnings;

use File::Find;
use File::stat;
use Fcntl ':mode';

my @files;

sub _is_perl_exec
{
	my $name = shift;
	my $out = `file $name 2>/dev/null`;
	return $out =~ /:.*perl[0-9]*\b/i;
}

sub findperl
{
	my @files ;
	my $wanted = sub
	{
		my $name = $File::Find::name;
		my $st;
		# check it's a plain file and either it has a perl extension (.p[lm])
		# or it's executable and `file` thinks it's a perl script.
		($st = lstat($_))
		  && -f $st
		  && (/\.p[lm]$/ || ((($st->mode & S_IXUSR) && _is_perl_exec($_))))
		  && push(@files, $name);
	};

	File::Find::find({ wanted => $wanted }, @_);
	return @files;
}

1;
