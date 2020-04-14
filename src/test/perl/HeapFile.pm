
=pod

=head1 NAME

HeapFile - definitions for tying arrays to PostgreSQL heap files.

=head1 SYNOPSIS

  use HeapFile;
  use HeapPage;

  my @immutable_file;
  tie @immutable_file, 'HeapFile',
    path => "/path/to/pgdata/heap/file",
    pagesize => 8192,
    mode => 'O_RDONLY';

  for my $immutable_page (@immutable_file)
  {
    print $immutable_page->{xlogid}, "\n";
  }

  my @mutable_file;
  tie @mutable_file, 'HeapFile',
    path => "/path/to/pgdata/heap/file",
    pagesize => 8192,
    mode => 'O_RDWR';

  # Bad ideas about managing your postgres data
  for my $page (@mutable_file)
  {
    $page->{pd_checksum} = 0;
    for my $tuple (@{$page->{tuples}})
    {
      $tuple->{HEAP_HASOID_OLD} = 0;
      $tuple->{t_xmin} = $tuple->{t_xmax}
        if ($tuple->{HEAP_XMIN_COMMITTED});
    }
  }

  # Make sure all changes have been written to disk
  untie @mutable_file;

=head1 DESCRIPTION

BEWARE: This module has nothing to do with tying perl arrays to a database for
the purposes of storing and retrieving user data.  This module is a debugging
tool for users who want to view and potentially change the data files that
underly a PostgreSQL cluster for debugging or recovering data.

This module provides a mechanism for array-tying a PostgreSQL heap file (or a
file pretending to be one) to a perl array.

An array tied to a PostgreSQL heap file will contain as many elements as there
are pages within the file.  Each element will be a HeapPage tied hash.  As
such, this class, and its accompanying HeapPage and HeapTuple helper classes,
implement a tied array of tied hashes.

Arrays tied in O_RDONLY mode will only read the heap file, and though
modifications may succeed in memory, they will not be written back to the file.
Arrays tied in O_RDWR mode will read the heap file, and modifications will be
made both in memory and in the disk file.

=cut

package HeapFile;

use strict;
use warnings;
use Tie::Array;
use HeapPage;
use Fcntl qw(SEEK_SET SEEK_CUR SEEK_END O_RDONLY O_RDWR);

our @ISA = qw(Tie::Array);

# Pack 8192 4-byte zeros for a total of 32k.  This is large enough for
# a zero page of any supported size.
my $ZEROS = pack("I[8192]", map { 0 } (1..8192));

# Constructor.  For inheritability, most work is performed in _init() rather
# than here.  We also don't document this function in POD, as users should
# be getting here through the tie interface, not through invoking new().
#
sub new
{
	my $thing = shift;
	my $class = ref($thing) || $thing;
	my $self = { };

	bless $self, $class;
	$self->_init(@_);

	return $self;
}

sub _init
{
	my ($self, %params) = @_;
	my $classname = ref($self) || "HeapFile";
	foreach my $param (qw(path))
	{
		die "In $classname: Missing required parameter '$param'"
			unless defined $params{$param};
	}
	$self->{path} = delete $params{path};
	$self->{pagesize} = delete $params{pagesize};
	$self->{pagesize} = HeapPage::DEFAULT_PAGESIZE unless(defined $self->{pagesize});
	$self->{pageno} = delete $params{pageno};
	$self->{mode} = delete $params{mode};
	$self->{mode} = O_RDONLY unless defined $self->{mode};
	die sprintf("In $classname: Unrecognized parameters: %s",
				join(', ', sort keys %params))
		if (scalar keys %params);

	HeapPage::validate_pagesize($self->{pagesize});
	$self->{mode} = HeapPage::validate_file_mode($self->{mode});

	# Initialize all known fields.  These will get overwritten
	# when we read the page from disk
	$self->{pages} = [];
	$self->_read();
}

sub _read
{
	my ($self) = @_;

	# Open the file ourselves, rather than having HeapPage open it repeatedly,
	# as we want the filesystem to give us a consistent view of the file, even
	# if other processes are modifying it simultaneously.
	#
	# We're not really hardened against concurrent modification, so this is just
	# a half-measure, but it seems better than taking no precautions at all.
	# At least in O_RDONLY mode, we'll have a consistent view of the file as it
	# was when we opened it.
	my $fh;
	sysopen($fh, $self->{path}, O_RDONLY)
		or die "Cannot open $self->{path} for reading: $!";
	binmode($fh);
	sysseek($fh, 0, SEEK_SET);
	my $fsize = (stat($fh))[7];
	for (my $pageno = 0; $fsize > 0; $pageno++)
	{
		my %page;
		tie %page, 'HeapPage',
			path => $self->{path},
			pagesize => $self->{pagesize},
			pageno => $pageno,
			mode => $self->{mode},
			fh => $fh;
		push (@{$self->{pages}}, \%page);
		$fsize -= $self->{pagesize};
	}
}

=pod

=head1 METHODS

=over

=item TIEARRAY classname, OPTIONS

The method invoked by the command "tie @array, 'HeapFile'".

OPTIONS is a list of name => value pairs, as follows:

=over 4

=item path

Required.  The filesystem path (relative or absolute) to the heap file.
Typically, this will be a path into a stopped PostgreSQL cluster's
PGDATA directory, something like data/base/12922/3456.

The specified file must exist, and the caller must have permission to
open the file in the specified mode.  No facility is provided to create
new heap files through this module.

=item mode

Optional.  The mode to use when opening the underlying heap file.  Valid
options are a subset of modes defined by Fcntl, specifically:

Defaults to O_RDONLY.

=over 4

=item O_RDONLY

Ties the array in read-only mode.  The underlying file will be read but not
modified.

=item O_RDWR

Ties the array in read-write mode.  The underlying file will be read, and
any changes to the array will be written back to the file.

=back

For callers who have not 'used' Fcntl, these modes can be specified as quoted
strings rather than as numerical constants.  Both forms are accepted.

=item pagesize

Optional.  The page size used when PostgreSQL was configured.  Defaults
to 8192 (8 kilobytes), which is the same as the default that PostgreSQL's
configure program uses.  Using a page size other than the one the cluster
was configured for may result in considerable confusion, and if opened
in O_RDWR mode, may also cause corruption when the array is modified or
untied and consequently written back to disk.

=back

=cut

sub TIEARRAY
{
	my $classname = shift;
	my $self = $classname->new(@_);
}

=pod

=item FETCH this, index

The method invoked when a field in the tied array is read.

Returns a reference to the tied hash for the page at the given index, or undef
if the index is beyond the end of the file.  The tied page belongs to the
HeapFile tied array, not to the caller.  The caller is at liberty to read
and modify the fields of the tied page, but should be careful not to untie
the page, as the resulting behavior if the page is untied is undefined.

=cut

sub FETCH
{
	my ($self, $index) = @_;
	return $self->{pages}->[$index];
}

=pod

=item FETCHSIZE this

The method invoked when calling scalar(@array) on the tied array.

Returns the number of pages in the tied array.

=cut

sub FETCHSIZE
{
	my ($self) = @_;
	return scalar(@{$self->{pages}});
}

=pod

=item STORE this, index, value

The method is invoked when a field in the tied array is written or modified.

If the value is a reference to a hash with an appropriate structure, that
structure will be assigned into the HeapPage tied hash at the given location
in the array.  Fields not specified will be left unaltered, or if no tied
hash yet exists at the specified array index, will be initialized as zero.
Any unrecognized keys or inappropriate values in the argument will draw an
exception and the tied array will be unaltered.

=cut

sub STORE
{
	my ($self, $index, $value) = @_;

	# Check that the argument value is valid before modifying our array
	# of HeapPages.  We don't want to perform a partial modification
	# and then throw an exception, as it would leave us in an inconsistent
	# state if the exception were caught.
	HeapPage::validate_hash($value);

	# If the index is beyond our array bounds, extend the heap file one
	# page at a time with zero pages until it is big enough.  This includes
	# creating a zero page for the target page.  We'll update the target
	# fields within the page after creating the zero page.
	my $pageno;
	for ($pageno = scalar(@{$self->{pages}}); $pageno <= $index; $pageno++)
	{
		my %newpage;
		tie %newpage, 'HeapPage',
			path => $self->{path},
			pagesize => $self->{pagesize},
			pageno => $pageno,
			mode => $self->{mode};
		die "Overwriting previous value!"
			if defined $self->{pages}->[$pageno];
		$self->{pages}->[$pageno] = \%newpage;
	}

	# Check that the target page now exists in our array
	die "No page allocated"
		unless defined $self->{pages}->[$index];
	my $target = $self->{pages}->[$index];

	# Overwrite the target page with the fields we were given, leaving all
	# unspecified fields zero
	$target->{$_} = $value->{$_} for(keys %$value);
}

=pod

=item STORESIZE this, newsize

This method is invoked to make the tied array longer or shorter.

Setting the length longer than the number of pages in the underlying file
will cause zero pages to be created at the end of the array.  Setting the
length shorter will truncate pages from the end of the array.  Whether this
happens only in memory or also on disk depends on whether the array was
tied in O_RDONLY or O_RDWR mode.

=cut

sub STORESIZE
{
	my ($self, $newsize) = @_;
	my $pageno;
	if ($newsize > scalar(@{$self->{pages}}))
	{
		for ($pageno = scalar(@{$self->{pages}}); $pageno < $newsize; $pageno++)
		{
			my %newpage;
			tie %newpage, 'HeapPage',
				path => $self->{path},
				pagesize => $self->{pagesize},
				pageno => $pageno,
				mode => $self->{mode};
			die "Overwriting previous value!"
				if defined $self->{pages}->[$pageno];
			$self->{pages}->[$pageno] = \%newpage;
		}
	}
	elsif (scalar(@{$self->{pages}}) > $newsize)
	{
		while (scalar(@{$self->{pages}}) > $newsize)
		{
			my $hashref = pop(@{$self->{pages}});
			untie %$hashref;
		}
		# Simply untying the pages won't erase ones beyond the end of the array,
		# so we have to do extra work if we're in O_RDWR mode to shorten the
		# underlying heap file.
		if ($self->{mode} == O_RDWR)
		{
			my $fh;
			sysopen($fh, $self->{path}, O_RDWR)
				or die "Cannot open $self->{path} for truncating: $!";
			binmode($fh);
			sysseek($fh, 0, SEEK_SET);
			my $current_fsize = (stat($fh))[7];
			my $new_fsize = scalar(@{$self->{pages}}) * $self->{pagesize};
			if ($new_fsize < $current_fsize)
			{
				warn "Attempt to truncate $self->{path} down to $new_fsize bytes failed"
					unless (truncate($fh, $new_fsize));
			}
			else
			{
				warn "In PageFile::STORESIZE, expected new_fsize < current_fsize " .
					 "($new_fsize vs. $current_fsize)";
			}
			$fh->close();
		}
	}
}

=pod

=item CLEAR this

This method is invoked to remove all elements from the tied array.

Truncate all pages from the array.  Whether this happens only in memory or also
on disk depends on whether the array was tied in O_RDONLY or O_RDWR mode.

=cut

sub CLEAR
{
	my ($self) = @_;
	$self->STORESIZE(0);
}

=pod

=item PUSH this, value

This method is invoked to add an element to the end of the tied array.

Whether this happens only in memory or also on disk depends on whether the
array was tied in O_RDONLY or O_RDWR mode.

=cut

sub PUSH
{
	my ($self, $value) = @_;
	$self->STORE(scalar(@{$self->{pages}}), $value);
}

=pod

=item POP this

This method is invoked to remove an element from the end of the tied array.

Whether this happens only in memory or also on disk depends on whether the
array was tied in O_RDONLY or O_RDWR mode.

The returned value is not a tied HeapPage, but merely a clone of the data
from what was a tied HeapPage prior to being popped.  This is necessary, at
least in O_RDWR mode, as the file is shortened by the action, and therefore
the returned value no longer refers to an actual page on disk.  We could
perhaps return a tied HeapPage when in O_RDONLY mode, but it seems sensible
to handle both cases the same.

=cut

sub POP
{
	my ($self) = @_;

	return undef unless @{$self->{pages}};

	my $tied_page = pop(@{$self->{pages}});
	my %untied_page;
	$untied_page{$_} = $tied_page->{$_} for keys %$tied_page;
	untie %$tied_page;
	undef($tied_page);

	# Simply untying the page won't erase it from the file, so we have to do
	# extra work if we're in O_RDWR mode to shorten the underlying heap file.
	if ($self->{mode} == O_RDWR)
	{
		my $fh;
		sysopen($fh, $self->{path}, O_RDWR)
			or die "Cannot open $self->{path} for truncating: $!";
		binmode($fh);
		sysseek($fh, 0, SEEK_SET);
		my $current_fsize = (stat($fh))[7];
		my $new_fsize = scalar(@{$self->{pages}}) * $self->{pagesize};
		if ($new_fsize < $current_fsize)
		{
			warn "Attempt to truncate $self->{path} down to $new_fsize bytes failed"
				unless (truncate($fh, $new_fsize));
		}
		else
		{
			warn "In PageFile::POP, expected new_fsize < current_fsize " .
				 "($new_fsize vs. $current_fsize)";
		}
		$fh->close();
	}
	return \%untied_page;
}

=item SHIFT this

This method is invoked to remove an element from the beginning of the tied
array.

Whether this happens only in memory or also on disk depends on whether the
array was tied in O_RDONLY or O_RDWR mode.

The returned value is not a tied HeapPage, but merely a clone of the data from
what was a tied HeapPage prior to being shifted off the array.  This is
necessary, at least in O_RDWR mode, as the file is shortened by the action, and
therefore the returned value no longer refers to an actual page on disk.  We
could perhaps return a tied HeapPage when in O_RDONLY mode, but it seems
sensible to handle both cases the same.

=cut

sub SHIFT
{
	my ($self) = @_;

	return undef unless @{$self->{pages}};

	# Store the first page, for returning later
	my $tied_page = $self->{pages}->[0];
	my %untied_page;
	$untied_page{$_} = $tied_page->{$_} for keys %$tied_page;

	# If we're in O_RDWR mode, move the content of the file page-by-page to be
	# one page ealier than it started.  Then re-read the file to re-synchronize
	# our in-memory array.
	if ($self->{mode} == O_RDWR)
	{
		# Untie and destroy all pages we currently have in memory.  We don't
		# want the pages re-writing themselves to disk after we've manually
		# modified the file.
		while (@{$self->{pages}})
		{
			my $tied = pop(@{$self->{pages}});
			untie %$tied;
			undef $tied;
		}

		# Shift the file down one page
		my $fh;
		sysopen($fh, $self->{path}, O_RDWR)
			or die "Cannot open $self->{path} for truncating: $!";
		binmode($fh);
		sysseek($fh, 0, SEEK_SET);
		my $old_fsize = (stat($fh))[7];
		my $new_fsize = $old_fsize - $self->{pagesize};
		my ($result, $buffer, $readpos, $writepos);
		for ($readpos = $self->{pagesize}, $writepos = 0;
			 $readpos < $old_fsize;
			 $readpos += $self->{pagesize}, $writepos += $self->{pagesize})
		{
			sysseek($fh, $readpos, SEEK_SET);
			$result = sysread($fh, $buffer, $self->{pagesize});
			warn "Partial read" if ($result < $self->{pagesize});
			warn "Read failure: $!" unless defined $result;
			sysseek($fh, $writepos, SEEK_SET);
			$result = syswrite($fh, $buffer, $self->{pagesize});
			warn "Partial write" if ($result < $self->{pagesize});
			warn "Write failure: $!" unless defined $result;
		}
		warn "Attempt to truncate $self->{path} down to $new_fsize bytes failed"
			unless (truncate($fh, $new_fsize));
		$fh->close();

		# Re-read the file, restoring our in-memory array
		$self->_read();
	}
	else
	{
		# In O_RDONLY mode, we would like to change each HeapPage to have a pageno
		# one less than before.  We don't have a way of doing that, and it doesn't
		# actually matter, because in O_RDONLY mode the pageno won't be used after
		# the initial loading from disk, which has already happened.
		#
		# Shorten our array, leaving the pageno fields broken
		shift (@{$self->{pages}});
	}
	return \%untied_page;
}

=item UNSHIFT this, value

This method is invoked to insert an element at the beginning of the tied
array.

Whether this happens only in memory or also on disk depends on whether the
array was tied in O_RDONLY or O_RDWR mode.

=cut

sub UNSHIFT
{
	my ($self, $value) = @_;

	HeapPage::validate_hash($value);

	# If we're in O_RDWR mode, move the content of the file page-by-page to be
	# one page later than it started.  Then re-read the file to re-synchronize
	# our in-memory array.
	if ($self->{mode} == O_RDWR)
	{
		# Untie and destroy all pages we currently have in memory.  We don't
		# want the pages re-writing themselves to disk after we've manually
		# modified the file.
		while (@{$self->{pages}})
		{
			my $tied = pop(@{$self->{pages}});
			untie %$tied;
			undef $tied;
		}

		# Shift the file down one page
		my $fh;
		sysopen($fh, $self->{path}, O_RDWR)
			or die "Cannot open $self->{path} for truncating: $!";
		binmode($fh);
		sysseek($fh, 0, SEEK_SET);
		my $old_fsize = (stat($fh))[7];
		my $new_fsize = $old_fsize + $self->{pagesize};
		my ($result, $buffer, $readpos, $writepos);
		for ($readpos = 0, $writepos = $self->{pagesize};
			 $readpos < $old_fsize;
			 $readpos += $self->{pagesize}, $writepos += $self->{pagesize})
		{
			sysseek($fh, $readpos, SEEK_SET);
			$result = sysread($fh, $buffer, $self->{pagesize});
			warn "Partial read" if ($result < $self->{pagesize});
			warn "Read failure: $!" unless defined $result;
			sysseek($fh, $writepos, SEEK_SET);
			$result = syswrite($fh, $buffer, $self->{pagesize});
			warn "Partial write" if ($result < $self->{pagesize});
			warn "Write failure: $!" unless defined $result;
		}
		# For now, just fill in the first page with zeros.
		sysseek($fh, 0, SEEK_SET);
		$result = syswrite($fh, $ZEROS, $self->{pagesize});
		warn "Partial write" if ($result < $self->{pagesize});
		warn "Write failure: $!" unless defined $result;
		$fh->close();

		# Re-read the file, restoring our in-memory array, and vivifying
		# a zero page at the beginning
		$self->_read();

		# Copy the unshifted value into the first page
		my $target = $self->{pages}->[0];

		# Overwrite the target page with the fields we were given, leaving all
		# unspecified fields zero
		$target->{$_} = $value->{$_} for(keys %$value);
	}
	else
	{
		my %page;
		tie %page, 'HeapPage',
			path => $self->{path},
			pagesize => $self->{pagesize},
			pageno => 0,
			mode => $self->{mode},
			virtual => $value;
		unshift(@{$self->{pages}}, \%page);
	}
}

=pod

=back

=cut

1;
