
=pod

=head1 NAME

HeapPage - definitions for tying hashes to PostgreSQL heap pages.

=head1 SYNOPSIS

  use HeapPage;

  # Open the 36th 8k page in read-only mode
  my %immutable_page;
  tie %immutable_page, 'HeapPage',
    path => "/path/to/pgdata/heap/file",
    pagesize => 8192,
    pageno => 35,
    mode => 'O_RDONLY';

  # Open the first 8k page in read-write mode
  my %mutable_page;
  tie %mutable_page, 'HeapPage',
    path => "/path/to/pgdata/heap/page",
    pagesize => 8192,
    pageno => 0,
    mode => 'O_RDWR';

  # Print human readable contents of the pages
  print scalar(%immutable_page), "\n";
  print scalar(%mutable_page), "\n";

  # Read some header fields
  print "Checksum: ", $immutable_page{pd_checksum}, "\n";

  # Modify some header fields
  $mutable_page{pd_checksum} += 12;
  $mutable_page{pd_lsn_xlogid} = 123456;

  # Copy fields from one page into another, overwriting them there
  $mutable_page{$_} = $immutable_page{$_} for keys %immutable_page;

=head1 DESCRIPTION

BEWARE: This module has nothing to do with tying perl hashes to a database for
the purposes of storing and retrieving user data.  This module is a debugging
tool for users who want to view and potentially change the data files that
underly a PostgreSQL cluster for debugging or recovering data.  If you want to
store data in PostgreSQL through a perl tied hash, see for example Tie::DBI.

This module provides a mechanism for hash-tying a page from a PostgreSQL heap
file (or a file pretending to be one) to a perl hash.

The hash behaves like a regular perl hash, within limits.  Specifically, the
hash can only be used to read or write fields that actually exist within
PostgreSQL heap pages.  Attempts to read or write fields of any other name will
draw an exception.  Generally speaking, any operation on the hash which
preserves the data format of a heap page will succeed, but other operations
(such as adding or removing keys) will fail.

All HeapPage tied hashes must be tied to an existant file, and the caller must
have filesystem permissions (read or read+write) to open the file in the
specified mode.  Modifications to tied hashes opened O_RDONLY will succeed in
memory, but the modification will not be written to disk.  Modifications to
tied hashes opened in O_RDWR will succeed in memory and be written to disk,
overwriting the existing page within the file.  No file locking is performed.
Beware that using this module to tie pages belonging to a running postgresql
cluster may give undefined (or catastrophic) results.

Each page must be tied not only to a file, but to a specific page within that
file.  Once tied, the page cannot be relocated to a different file nor to a
different location within the same file, nor can the pagesize be altered.

=cut

package HeapPage;

use strict;
use warnings;
use Tie::Hash;
use IO::File;
use Carp;
use HeapTuple;
use Fcntl qw(SEEK_SET SEEK_CUR SEEK_END O_RDONLY O_RDWR);

our @ISA = qw(Tie::StdHash);

use constant SIZE_UINT16 => 2;
use constant MIN_UINT16 => 0;
use constant MAX_UINT16 => 2**16 - 1;

use constant SIZE_UINT32 => 4;
use constant MIN_UINT32 => 0;
use constant MAX_UINT32 => 2**32 - 1;

# Hard-code some constants from elsewhere in the PostgreSQL project.
# Be sure to update these if you change the core code.
use constant MAX_BLOCK_NUMBER => 0xFFFFFFFE;	# From storage/block.h
use constant DEFAULT_PAGESIZE => 8192;			# From configure.in

use constant LP_UNUSED => 0;		# From storage/itemid.h
use constant LP_NORMAL => 1;		# From storage/itemid.h
use constant LP_REDIRECT => 2;		# From storage/itemid.h
use constant LP_DEAD => 3;			# From storage/itemid.h

# Hard-code some information about the PageHeaderData struct.  We need to read
# and write binary copies of this, using perl's pack() and unpack() functions,
# but at least we can avoid scattering our assumptions about the format of the
# PageHeaderData structure throughout the module and instead declare them here
# in one place.
#
# pd_lsn:  struct
#	xlogid:				uint32		=> L (Unsigned 32-bit Integer)	/ 4 bytes
#	xrecoff:			uint32		=> L (Unsigned 32-bit Integer)	/ 4 bytes
# pd_checksum:			uint16		=> S (Unsigned 16-bit Short)	/ 2 bytes
# pd_flags:				uint16		=> S (Unsigned 16-bit Short)	/ 2 bytes
# pd_lower:				uint16		=> S (Unsigned 16-bit Short)	/ 2 bytes
# pd_upper:				uint16		=> S (Unsigned 16-bit Short)	/ 2 bytes
# pd_special:			uint16		=> S (Unsigned 16-bit Short)	/ 2 bytes
# pd_pagesize_version:	uint16		=> S (Unsigned 16-bit Short)	/ 2 bytes
# pd_prune_xid:			uint32		=> L (Unsigned 32-bit Integer)	/ 4 bytes
#                                                                  ----------
#                                                         Total      24 bytes
#
use constant PAGEHEADER_PACK_CODE => 'LLSSSSSSL';
use constant PAGEHEADER_PACK_LENGTH => 24;		# Total size

# The names of page header fields.  If you modify this list, also modify the
# tied hash function 'sub SCALAR' below, and %PageHeaderBytes and
# %PageHeaderRange also.
#
# THESE MUST BE IN PAGE ORDER!
#
# Note that we treat the two subfields of pd_lsn as if they were top level
# fields, and name them as such.  This avoids needing to have nested tied
# hashes, which seems like it would be more work than it is worth.
#
our @PageHeaderKeys = qw(
	pd_lsn_xlogid pd_lsn_xrecoff pd_checksum pd_flags pd_lower pd_upper
	pd_special pd_pagesize_version pd_prune_xid
);

# All PageHeaderKeys must have entries here.  It is tempting to use the size of
# the field to define the [min..max] range, but given the differences between
# signed and unsigned types, and the option to use less than all bits in a
# field, we keep a separate list of [min..max] in %PageHeaderRange, below.
our %PageHeaderBytes = (
	pd_lsn_xlogid => SIZE_UINT32,
	pd_lsn_xrecoff => SIZE_UINT32,
	pd_checksum => SIZE_UINT16,
	pd_flags => SIZE_UINT16,
	pd_lower => SIZE_UINT16,
	pd_upper => SIZE_UINT16,
	pd_special => SIZE_UINT16,
	pd_pagesize_version => SIZE_UINT16,
	pd_prune_xid => SIZE_UINT32,
);
our %PageHeaderMask = (
	pd_lsn_xlogid => 0xFFFFFFFF,
	pd_lsn_xrecoff => 0xFFFFFFFF,
	pd_checksum => 0xFFFF,
	pd_flags => 0xFFFF,
	pd_lower => 0xFFFF,
	pd_upper => 0xFFFF,
	pd_special => 0xFFFF,
	pd_pagesize_version => 0xFFFF,
	pd_prune_xid => 0xFFFFFFFF,
);
our %PageHeaderRange = (
	pd_lsn_xlogid => [MIN_UINT32, MAX_UINT32],
	pd_lsn_xrecoff => [MIN_UINT32, MAX_UINT32],
	pd_checksum => [MIN_UINT16, MAX_UINT16],
	pd_flags => [MIN_UINT16, MAX_UINT16],
	pd_lower => [MIN_UINT16, MAX_UINT16],
	pd_upper => [MIN_UINT16, MAX_UINT16],
	pd_special => [MIN_UINT16, MAX_UINT16],
	pd_pagesize_version => [MIN_UINT16, MAX_UINT16],
	pd_prune_xid => [MIN_UINT32, MAX_UINT32],
);

our @PageKeys = (@PageHeaderKeys, "linepointers");
our %PageKeys = map { $_ => 1 } @PageKeys;

#
# Module utility functions
#

sub integer_in_range
{
	my ($int, $min, $max) = @_;
	return (defined $int && $int =~ m/^\d+$/ && $int >= $min && $int <= $max);
}

sub integer_in_list
{
	my ($int, @values) = @_;
	return unless (defined $int && $int =~ m/^\d+$/);
	for (@values)
	{
		return 1 if ($int == $_);
	}
}

sub validate_hash
{
	my ($hashref) = @_;

	die "Not a hash ref"
		unless(defined $hashref && ref($hashref) && ref($hashref) =~ m/^HASH$/);
	foreach my $key (keys %$hashref)
	{
		my $value = $hashref->{$key};

		die "Attempt to store unrecognized HeapPage key $key"
			unless exists $PageKeys{$key};
		die "cannot store undefined value for $key"
			unless defined $value;

		if (exists $PageHeaderRange{$key})
		{
			my ($min, $max) = @{$PageHeaderRange{$key}};
			die "$key:$value not within supported range [$min..$max]"
				unless integer_in_range($value, $min, $max);
		}
	}
}

sub validate_pagesize
{
	my $pagesize = shift;
	die "Invalid pagesize: $pagesize"
		unless integer_in_list($pagesize, (2**11, 2**12, 2**13, 2**14, 2**15));
}

sub validate_pageno
{
	my $pageno = shift;
	die "Invalid pageno: $pageno"
		unless integer_in_range($pageno, 0, MAX_BLOCK_NUMBER);
}

sub validate_file_mode
{
	my $mode = shift;
	return O_RDONLY if ($mode eq 'O_RDONLY');
	return O_RDWR if ($mode eq 'O_RDWR');
	return $mode if ($mode == O_RDONLY || $mode == O_RDWR);

	die "Invalid or unsupported file mode";
}

sub _pageheader_pack
{
	my ($self) = @_;

	my $pack = pack(PAGEHEADER_PACK_CODE,
					$self->{pagedata}->{pd_lsn_xlogid},
					$self->{pagedata}->{pd_lsn_xrecoff},
					$self->{pagedata}->{pd_checksum},
					$self->{pagedata}->{pd_flags},
					$self->{pagedata}->{pd_lower},
					$self->{pagedata}->{pd_upper},
					$self->{pagedata}->{pd_special},
					$self->{pagedata}->{pd_pagesize_version},
					$self->{pagedata}->{pd_prune_xid});

	# sanity check -- make sure it round trips ok.  This is not
	# a test of Perl's pack mechanism, but of the assumption that
	# none of our fields will be truncated/altered when using the
	my @unpack = unpack(PAGEHEADER_PACK_CODE, $pack);
	die "_pageheader_pack does not roundtrip"
		unless ($unpack[0] == $self->{pagedata}->{pd_lsn_xlogid} &&
				$unpack[1] == $self->{pagedata}->{pd_lsn_xrecoff} &&
				$unpack[2] == $self->{pagedata}->{pd_checksum} &&
				$unpack[3] == $self->{pagedata}->{pd_flags} &&
				$unpack[4] == $self->{pagedata}->{pd_lower} &&
				$unpack[5] == $self->{pagedata}->{pd_upper} &&
				$unpack[6] == $self->{pagedata}->{pd_special} &&
				$unpack[7] == $self->{pagedata}->{pd_pagesize_version} &&
				$unpack[8] == $self->{pagedata}->{pd_prune_xid});

	return $pack;
}

sub _pageheader_unpack
{
	my ($self, $packed) = @_;
	return unpack(PAGEHEADER_PACK_CODE, $packed);
}

sub _linepointer_encode
{
	my ($lp_off, $lp_flags, $lp_len) = @_;
	my $uint32 = ($lp_len << 17) |
				 ($lp_flags << 15) |
				 ($lp_off);
	return $uint32;
}

sub _linepointer_decode
{
	my ($uint32) = @_;
	my $lp_off = $uint32 & 0x7FFF;
	my $lp_flags = ($uint32 >> 15) & 0x03;
	my $lp_len = ($uint32 >> 17) & 0x7FFF;
	return ($lp_off, $lp_flags, $lp_len);
}

sub _linepointers_pack
{
	my ($self) = @_;

	# my $test = _linepointer_encode(5, 1, 7);
	# my @test = _linepointer_decode($test);
	# die "Encoded (5,1,7), got back @test";

	my @linewords;
	foreach my $linepointer (@{$self->{pagedata}->{linepointers}})
	{
		my $uint32 = _linepointer_encode($linepointer->{lp_off},
										 $linepointer->{lp_flags},
										 $linepointer->{lp_len});

		# Debugging
		my ($a, $b, $c) = _linepointer_decode($uint32);
		die sprintf("linepointers did not round-trip: (%u,%u,%u) => (%u,%u,%u)",
						$linepointer->{lp_off},
						$linepointer->{lp_flags},
						$linepointer->{lp_len},
						$a, $b, $c)
			unless($a == $linepointer->{lp_off} &&
				   $b == $linepointer->{lp_flags} &&
				   $c == $linepointer->{lp_len});

		push (@linewords, $uint32);
	}
	my $wordlength = scalar(@linewords);
	my $packed = pack("L[$wordlength]", @linewords);

	return ($wordlength*4, $packed);
}

sub _linepointers_unpack
{
	my ($self, $packed, $bytelength) = @_;
	my @linepointers;

	die "_linepointers_unpack handed bytelength = $bytelength"
		if ($bytelength % 4 != 0);

	my $wordlength = $bytelength / 4;

	my @linewords = unpack("L[$wordlength]", $packed);
	foreach my $lineword (@linewords)
	{
		my ($lp_off, $lp_flags, $lp_len) = _linepointer_decode($lineword);
		push (@linepointers, { lp_off => $lp_off,
							   lp_flags => $lp_flags,
							   lp_len => $lp_len });
	}
	$self->{pagedata}->{linepointers} = \@linepointers;
}

sub _init
{
	my ($self, %params) = @_;
	my $classname = ref($self) || "HeapPage";

	foreach my $param (qw(path pageno))
	{
		die "In $classname: Missing required parameter '$param'"
			unless defined $params{$param};
	}
	$self->{path} = delete $params{path};
	$self->{pagesize} = delete $params{pagesize};
	$self->{pagesize} = DEFAULT_PAGESIZE unless(defined $self->{pagesize});
	$self->{pageno} = delete $params{pageno};
	$self->{mode} = delete $params{mode};
	$self->{mode} = O_RDONLY unless defined $self->{mode};

	validate_pagesize($self->{pagesize});
	validate_pageno($self->{pageno});
	$self->{mode} = validate_file_mode($self->{mode});

	$self->{seekto} = $self->{pagesize} * $self->{pageno};

	# Initialize all known fields.  These will get overwritten
	# when we read the page from disk or copy it from our virtual argument
	$self->{pagedata} =
		{
			pd_lsn_xlogid => 0,
			pd_lsn_xrecoff => 0,
			pd_checksum => 0,
			pd_flags => 0,
			pd_lower => 0,
			pd_upper => 0,
			pd_special => 0,
			pd_pagesize_version => 0,
			pd_prune_xid => 0,
		};
	$self->{tuples} = [];

	if (exists $params{virtual})
	{
		$self->{pagedata}->{$_} = $params{virtual}->{$_}
			for keys %{$params{virtual}};
	}
	else
	{
		my $fh = delete $params{fh};
		die sprintf("In $classname: Unrecognized parameters: %s",
					join(', ', sort keys %params))
			if (scalar keys %params);
		$self->_read($fh);
	}
}

sub _read
{
	my ($self, $fh) = @_;

	# If the caller handed us an open file handle, use that, otherwise open
	# the file ourselves.
	my $did_open = 0;
	unless (defined $fh)
	{
		sysopen($fh, $self->{path}, O_RDONLY)
			or die "Cannot open $self->{path} for reading: $!";
		binmode($fh);
		$did_open = 1;
	}
	my ($result, $rawdata, $zeropage, $incompletepage);
	sysseek($fh, $self->{seekto}, SEEK_SET);
	$result = sysread($fh, $rawdata, PAGEHEADER_PACK_LENGTH);

	# We attempted to read the entire page, but we really only have trouble
	# if we got less than the header.  This will change, as eventually
	# we'll implement processing of the entire page.
	if ($result > 0 && $result < PAGEHEADER_PACK_LENGTH)
	{
		warn("_read: Read partial page header: Expected " .
				PAGEHEADER_PACK_LENGTH .
				" bytes, but got $result");
		$incompletepage = 1;
	}
	elsif ($result == 0)
	{
		warn("_read: Read beyond end of heap page file: simulating read " .
			 "of an all-zero page");
		$zeropage = 1;
	}
	elsif (! defined $result)
	{
		die "Cannot read from $self->{path}: $!";
	}

	# Unpack the page header fields into perl scalars
	my @unpacked = $self->_pageheader_unpack($rawdata);

	# Sanity-check and store the page header fields, filling in
	# zeroes for empty or incomplete page reads
	foreach my $key (@PageHeaderKeys)
	{
		my $value = shift @unpacked;
		$value = 0 if ($zeropage);
		die "HeapPage unpacked fewer header fields than expected"
			unless (defined $value || $incompletepage);
		$value = 0 unless defined $value;
		$value &= $PageHeaderMask{$key}
			if (exists $PageHeaderMask{$key});
		validate($key, $value);
		$self->{pagedata}->{$key} = $value;
	}
	die "HeapPage unpacked more header fields than expected"
		if scalar @unpacked;

	# Read the line pointers, if possible.  We need to handle corrupted pages,
	# so be careful to only read them if the page header is sensible.  We read
	# and store line pointers even if they are unreasonable; we only skip them
	# if the tuple header would have us reading outside the bounds of the page.
	my $lplen = $self->{pagedata}->{pd_lower} - PAGEHEADER_PACK_LENGTH;
	if ($lplen < 0 || $lplen % 4 != 0)
	{
		warn "Corrupt header in page $self->{pageno}: Not reading line pointers";
		$self->{pagedata}->{linepointers} = [];
	}
	elsif ($lplen == 0)
	{
		warn "Header in page $self->{pageno} shows zero line pointers";
		$self->{pagedata}->{linepointers} = [];
	}
	else
	{
		sysseek($fh, $self->{seekto} + PAGEHEADER_PACK_LENGTH, SEEK_SET);
		$result = sysread($fh, $rawdata, $lplen);
		die "line pointer partial read" if ($result > 0 && $result < $lplen);
		die "line pointer read error" if ($result == 0);
		die "line pointer read error: $!" unless defined $result;
		$self->_linepointers_unpack($rawdata, $lplen);

		# Debugging
		my ($test_bytelength, $test_packed) = $self->_linepointers_pack();
		warn "test_bytelength != lplen ($test_bytelength != $lplen)"
			if ($test_bytelength != $lplen);
		warn "test_packed != rawdata" if ($test_packed ne $rawdata);
	}

	# Read the tuples, if possible.  We need to handle corrupted pages, so be
	# careful to only read tuples that are within sensible bounds on the page.
	# Because we were lax about the exact contents of the line pointers above,
	# we have to be ready for the line pointers to be unreasonable.
	if ($self->{pagedata}->{pd_lower} <= $self->{pagedata}->{pd_upper} &&
		$self->{pagedata}->{pd_upper} <= $self->{pagedata}->{pd_special} &&
		$self->{pagedata}->{pd_special} <= $self->{pagesize})
	{
		foreach my $linepointer (@{$self->{pagedata}->{linepointers}})
		{
			my $lp_flags = $linepointer->{lp_flags};
			my $lp_off = $linepointer->{lp_off};
			my $lp_len = $linepointer->{lp_len};
			if ($lp_flags == LP_NORMAL)
			{
				if ($lp_off >= $self->{pagedata}->{pd_upper} &&
					$lp_off + $lp_len <= $self->{pagedata}->{pd_special})
				{
					my %tuple;
					sysseek($fh, $self->{seekto} + $lp_off, SEEK_SET);
					tie %tuple, 'HeapTuple',
								fh => $fh,
								lp_len => $lp_len,
								lp_off => $lp_off;
					push (@{$self->{tuples}}, \%tuple);
				}
				else
				{
					carp ("Skipping tuple due to violation of invariant: " .
							"lp_off >= pd_upper && lp_off + lp_len <= pd_special");
				}
			}
		}
	}
	else
	{
		carp sprintf("Skipping tuples on page due to violation of invariant: " .
					 "pd_lower %u <= pd_upper %u <= pd_special %u <= pagesize %u",
						$self->{pagedata}->{pd_lower},
						$self->{pagedata}->{pd_upper},
						$self->{pagedata}->{pd_special},
						$self->{pagesize},
						);
	}

	close($fh) if $did_open;
}

sub _write
{
	my ($self) = @_;

	die "Attempt to _write tied HeapPage that was tied in O_RDONLY mode"
		if ($self->{mode} == O_RDONLY);

	# Pack the page header fields into bytes to write to the heap page file
	my $packed = $self->_pageheader_pack();

	# Write the page header
	my $fh;
	sysopen($fh, $self->{path}, O_WRONLY)
		or die "Cannot open $self->{path} for writing: $!";
	binmode($fh);
	sysseek($fh, $self->{seekto}, SEEK_SET);
	syswrite($fh, $packed, PAGEHEADER_PACK_LENGTH);

	# Write the line pointers
	my $bytelength;
	($bytelength, $packed) = $self->_linepointers_pack();
	sysseek($fh, $self->{seekto} + PAGEHEADER_PACK_LENGTH, SEEK_SET);
	syswrite($fh, $packed, $bytelength);

	# Write the tuples
	for my $tuple (@{$self->{tuples}})
	{
		# Our job to seek to the beginning of the page
		sysseek($fh, $self->{seekto}, SEEK_SET);

		# Tuple's job to seek to the tuple's lp_off
		my $sub = $tuple->{SERIALIZATION_CLOSURE};
		$sub->($fh);
	}

	close($fh);
}

sub validate
{
	my ($key, $value) = @_;
	my $range = $PageHeaderRange{$key}
		or die "Attempt to validate unrecognized key: $key";
	die "Invalid value for $key: $value not in $range->[0] .. $range->[1]"
		unless (integer_in_range($value, @$range));
}

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

=pod

=head1 METHODS

=over

=item TIEHASH classname, OPTIONS

The method invoked by the command "tie %hash, 'HeapPage'".

OPTIONS is a list of name => value pairs, as follows:

=over 4

=item path

Required.  The filesystem path (relative or absolute) to the heap file.
Typically, this will be a path into a stopped PostgreSQL cluster's
PGDATA directory, something like data/base/12922/3456.

The specified file must exist, and the caller must have permission to
open the file in the specified mode.  No facility is provided to create
new heap files through this module.

=item pageno

Required.  The number of the page within the file.  Page numbers start
at zero.  If the page is beyond the end of the file, an initially all-zero
page will be vivified for the purpose.  If the file is being tied in O_RDONLY
mode, the new page will be in-memory only and will not affect the file.  If,
however, the file is being tied in O_RDWR mode, the new page will ultimately
get written back out to the file, changing and extending the file.

=item mode

Optional.  The mode to use when opening the underlying heap file.  Valid
options are a subset of modes defined by Fcntl, specifically:

Defaults to O_RDONLY.

=over 4

=item O_RDONLY

Ties the hash in read-only mode.  The underlying file will be read but not
modified.

=item O_RDWR

Ties the hash in read-write mode.  The underlying file will be read, and
any changes to the hash will be written back to the file.

=back

For callers who have not 'used' Fcntl, these modes can be specified as quoted
strings rather than as numerical constants.  Both forms are accepted.

=item pagesize

Optional.  The page size used when PostgreSQL was configured.  Defaults
to 8192 (8 kilobytes), which is the same as the default that PostgreSQL's
configure program uses.  Using a page size other than the one the cluster
was configured for may result in considerable confusion, and if opened
in O_RDWR mode, may also cause corruption when the hash is modified or
untied and consequently written back to disk.

=back

=cut

sub TIEHASH
{
	my $classname = shift;
	my $self = $classname->new(@_);
}

=pod

=item UNTIE this

The method invoked by the command "untie %hash".

If the hash was tied in O_RDWR mode, untying the hash will result in the
contents of the hash being written to the underlying heap file.

=cut

sub UNTIE
{
	my ($self) = @_;
	$self->_write() if ($self->{mode} == O_RDWR);
}

=pod

=item DESTROY this

The mothod invoked when the tied hash gets garbage collected.

UNTIE's the hash.

=cut

sub DESTROY
{
	my ($self) = @_;
	$self->UNTIE();
}

=pod

=item STORE this, key, value

The method invoked when a field in the tied hash is written or modified.

Checks the key against the list of valid page field names and the value
against the supported range of the field, raising exceptions for invalid
key/value pairs.  For valid pairs, the hash is updated and, if tied in
O_RDWR mode, the file is written.

=cut

sub STORE
{
	my ($self, $key, $value) = @_;
	die "Attempt to store unrecognized HeapPage key $key"
		unless exists $PageKeys{$key};
	die "cannot store undefined value for $key"
		unless defined $value;

	$value &= $PageHeaderMask{$key}
		if (exists $PageHeaderMask{$key});
	if (exists $PageHeaderRange{$key})
	{
		my ($min, $max) = @{$PageHeaderRange{$key}};
		die "$key:$value not within supported range [$min..$max]"
			unless integer_in_range($value, $min, $max);
	}
	$self->{pagedata}->{$key} = $value;
	$self->_write() if ($self->{mode} == O_RDWR);
}

=pod

=item FETCH this, key

The method invoked when a field in the tied hash is read.

Checks the key against the list of valid page field names and raises an
exception for unrecognized fields.  For valid fields, returns the value last
read from the file.  The file is not re-checked; alterations by other programs
or processes will not immediately be noticed.

=cut

sub FETCH
{
	my ($self, $key) = @_;
	return $self->{tuples} if ($key eq 'tuples');
	die "Attempt to fetch unrecognized HeapPage key $key"
		unless exists $PageKeys{$key};
	return $self->{pagedata}->{$key};
}

=pod

=item FIRSTKEY this

The method invoked when beginning iteration over the hash keys as a result of
calling 'keys' or 'each' on the tied hash.

=cut

sub FIRSTKEY
{
	my ($self) = @_;
	$self->{keys} = [@PageKeys];	# Start new iteration
	return $self->NEXTKEY();
}

=pod

=item NEXTKEY this

The method invoked when continuing iteration over the hash keys as a result of
calling 'keys' or 'each' on the tied hash.

=cut

sub NEXTKEY
{
	my ($self) = @_;
	return shift(@{$self->{keys}});
}

=pod

=item EXISTS this, key

The method invoked when checking existence of a key within the tied hash.

The set of keys which exist is invariable, because it is defined by the format
of PostgreSQL heap pages.

=cut

sub EXISTS
{
	my ($self, $key) = @_;
	return exists $PageKeys{$key};
}

=pod

=item DELETE this, key

The method invoked when deleting a key from a tied hash.

This method always raises an exception, as the set of keys is static and
unalterable, governed by the fixed format of PostgreSQL heap pages.

=cut

sub DELETE
{
	my ($self, $key) = @_;
	die "Attempt to delete unrecognized HeapPage key $key"
		unless exists $PageKeys{$key};
	die "Operation not supported: Cannot delete keys from HeapPages: $key";
}

=pod

=item CLEAR this

The method invoked when trying to delete all keys from a tied hash.

This method unties and empties the hash.

=cut

sub CLEAR
{
	my ($self) = @_;
	$self->UNTIE();
	my @keys = keys %$self;
	delete $self->{$_} for (@keys);
}

=pod

=item SCALAR this

The method invoked when evaluating the hash in scalar context.

Returns a string containing a single line of human readable text representing the
page header fields and values.

=back

=cut

# These strings are intentionally all the same length
our %lp_flags_str = (
	LP_UNUSED   => "UNUSED   ",
	LP_NORMAL   => "NORMAL   ",
	LP_REDIRECT => "REDIRECT ",
	LP_DEAD     => "DEAD     ",
);

sub SCALAR
{
	my ($self) = @_;
	my @lines;
	push (@lines, sprintf("PAGE:%u pd_lsn.xlogid:%u pd_lsn.xrecoff:%u " .
							"pd_checksum:%u pd_flags:%u pd_lower:%u " .
							"pd_upper:%u pd_special:%u " .
							"pd_pagesize_version:%u pd_prune_xid:%u",
					$self->{pageno},
					$self->{pagedata}->{pd_lsn_xlogid},
					$self->{pagedata}->{pd_lsn_xrecoff},
					$self->{pagedata}->{pd_checksum},
					$self->{pagedata}->{pd_flags},
					$self->{pagedata}->{pd_lower},
					$self->{pagedata}->{pd_upper},
					$self->{pagedata}->{pd_special},
					$self->{pagedata}->{pd_pagesize_version},
					$self->{pagedata}->{pd_prune_xid}));
	foreach my $lineptr (@{$self->{pagedata}->{linepointers}})
	{
		push (@lines, sprintf("    [%s lp_off:% 5u lp_len:% 5u]",
					$lp_flags_str{$lineptr->{lp_flags}},
					map { $lineptr->{$_} } qw(lp_off lp_len)));
	}
	foreach my $tuple (@{$self->{tuples}})
	{
		push (@lines, scalar(%$tuple));
	}
	return join("\n", @lines);
}

1;
