
=pod

=head1 NAME

HeapTuple - perl module for representing the contents of a heap tuple.

=head1 SYNOPSIS

  use HeapFile;
  use HeapPage;
  use HeapTuple;

  # Use the HeapFile interface in user code
  tie @file, 'HeapFile',
    path => "/path/to/pgdata/heap/file",
    pagesize => 8192,
    mode => 'O_RDWR';

  # Iterate over all pages in the heap file, changing the 't_xmin"
  # field:
  for my $page (@file)
  {
    for my $tuple (@{$page->{tuples}})
    {
      print "OLD_XMIN: ", $tuple->{t_xmin}, "\n";
      $tuple->{t_xmin}++;
      print "NEW_XMIN: ", $tuple->{t_xmin}, "\n";
    }
  }

  # There was a little magic above, as the tuple hash does not have
  # a "t_xmin" field.  If you iterated over keys(%$tuple), you
  # wouldn't encounter any t_xmin.  The structure looks like this:
  #
  #   t_choice => {
  #     t_heap => {
  #       t_xmin => UINT32,
  #       t_xmax => UINT32,
  #       t_field3 => UINT32,
  #     },
  #   },
  #   t_ctid => {
  #     ip_blkid => {
  #       bi_hi => UINT16,
  #       bi_lo => UINT16,
  #     },
  #     ip_posid => UINT16,
  #   },
  #   t_infomask2 => UINT16,
  #   t_infomask => UINT16
  #   t_hoff => UINT8,
  #
  # But since this is a tied hash, it can do things ordinary hashes
  # cannot, and in this instance it read and updated
  # $tuple->{t_choice}->{t_heap}->{t_xmin} as a convenience.
  #
  # We can also update bit fields by name, using the naming
  # conventions from htup_details.h.  The appropriate field for the
  # name will be read or written automatically:

  my $a = $heapfile_a[17]->{tuples}->[3];
  my $b = $heapfile_b[17]->{tuples}->[17];

  # Only copy the HEAP_HASNULL bit, not the whole t_infomask:
  $a->{HEAP_HASNULL} = $b->{HEAP_HASNULL};

  # Copy all the bits of t_infomask2 masked by HEAP_NATTS_MASK
  $a->{HEAP_NATTS_MASK} = $b->{HEAP_NATTS_MASK};

  # Copy t_heap field in its entirety:
  $a->{t_heap} = $b->{t_heap};

  # Read the body of the tuple (the bytes starting from t_hoff):
  my @octets = $a->{PAYLOAD_CHR};

  # Same thing, but as hex
  my @hex = $a->{PAYLOAD_HEX};

=head1 DESCRIPTION

Each tuple in a HeapPage is represented as a HeapTuple tied hash.

The tied hash is a rigid structure, refusing to allow fields to be
added or removed, but allowing modifications within reason.
Attempts to set a field to a value too large for the number of bits
reserved for the field will result in truncation of the value.
Tuples cannot be moved from one HeapPage to another (whether in the
same file or a different file), nor can they be relocated on the
same page.  Values from one tuple may be assigned to another tuple,
of course, though the assignment is by value, and does not affect
the original tuple.

=head1 LIMITATIONS

The payload of the tuple can be read, but the structure of the data
is unknown, so it comes back as octets or hexadecimal characters, at
your option.  It cannot be written.

This limitation may be eased in the future.

=cut

package HeapTuple;

use strict;
use warnings;
use Tie::Hash;
use Data::Dumper;
use Fcntl qw(SEEK_SET SEEK_CUR SEEK_END O_RDONLY O_RDWR);
use POSIX;
use Carp;

our @ISA = qw(Tie::StdHash);

use constant SIZE_UINT8 => 1;
use constant MIN_UINT8 => 0;
use constant MAX_UINT8 => 2**8 - 1;

use constant SIZE_UINT16 => 2;
use constant MIN_UINT16 => 0;
use constant MAX_UINT16 => 2**16 - 1;

use constant SIZE_UINT32 => 4;
use constant MIN_UINT32 => 0;
use constant MAX_UINT32 => 2**32 - 1;

use constant MaxTupleAttributeNumber => 1664;	# From access/htup_details.h
use constant MaxHeapAttributeNumber => 1600;	# From access/htup_details.h

use constant HEAP_HASNULL => 0x0001;
use constant HEAP_HASVARWIDTH => 0x0002;
use constant HEAP_HASEXTERNAL => 0x0004;
use constant HEAP_HASOID_OLD => 0x0008;
use constant HEAP_XMAX_KEYSHR_LOCK => 0x0010;
use constant HEAP_COMBOCID => 0x0020;
use constant HEAP_XMAX_EXCL_LOCK => 0x0040;
use constant HEAP_XMAX_LOCK_ONLY => 0x0080;
use constant HEAP_XMIN_COMMITTED => 0x0100;
use constant HEAP_XMIN_INVALID => 0x0200;
use constant HEAP_XMAX_COMMITTED => 0x0400;
use constant HEAP_XMAX_INVALID => 0x0800;
use constant HEAP_XMAX_IS_MULTI => 0x1000;
use constant HEAP_UPDATED => 0x2000;
use constant HEAP_MOVED_OFF => 0x4000;
use constant HEAP_MOVED_IN => 0x8000;

use constant HEAP_NATTS_MASK => 0x07FF;			# From access/htup_details.h
use constant HEAP_KEYS_UPDATED => 0x2000;		# From access/htup_details.h
use constant HEAP_HOT_UPDATED => 0x4000;		# From access/htup_details.h
use constant HEAP_ONLY_TUPLE =>  0x8000;		# From access/htup_details.h

#define HEAP2_XACT_MASK         0xE000  /* visibility-related bits */


our @TupleKeys = qw(t_choice t_ctid t_infomask2 t_infomask t_hoff t_bits);
our %TupleKeys = map { $_ => 1 } @TupleKeys;

# Hard-code some information about the HeapTupleHeaderData struct.  We need to
# read and write binary copies of this, using perl's pack() and unpack()
# functions, but at least we can avoid scattering our assumptions about the
# format of the HeapTupleHeaderData structure throughout the module and instead
# declare them here in one place.
#
# t_choice:		union
#	t_heap:		struct
#		t_xmin:			uint32		=> L (Unsigned 32-bit Integer)	/ 4 bytes
#		t_xmax:			uint32		=> L (Unsigned 32-bit Integer)	/ 4 bytes
#		t_field3:	union
#			t_cid:		uint32		=> L (Unsigned 32-bit Integer)	/ 4 bytes
#		  OR
#			t_xvac:		uint32		=>				ditto
# t_ctid:		struct
#	ip_blkid:	struct
#		bi_hi			uint16		=> S (Unsigned 16-bit Short)	/ 2 bytes
#		bi_lo			uint16		=> S (Unsigned 16-bit Short)	/ 2 bytes
#	ip_posid:			uint16		=> S (Unsigned 16-bit Short)	/ 2 bytes
# t_infomask2:			uint16		=> S (Unsigned 16-bit Short)	/ 2 bytes
# t_infomask:			uint16		=> S (Unsigned 16-bit Short)	/ 2 bytes
# t_hoff:				uint8		=> C (Unsigned  8-bit Octet)	/ 1 byte
#                                                                  ----------
#                                                         Total      23 bytes
#
use constant HEAPTUPLEHEADER_PACK_CODE => 'LLLSSSSSC';
use constant HEAPTUPLEHEADER_PACK_LENGTH => 23;		# Total size

sub integer_in_range
{
	my ($int, $min, $max) = @_;
	return (defined $int && $int =~ m/^\d+$/ && $int >= $min && $int <= $max);
}

# Examines the (nested) structure of two hash references to see that their
# hash structures are the same.  Ignores scalars and array references, but
# recurses on hash reference values.
sub hash_has_format
{
	my ($href, $format) = @_;

	return unless (defined $href && ref $href && ref($href) =~ m/HASH/ &&
					defined $format && ref $format && ref($format) =~ m/HASH/);

	my $a = join(' ', sort keys %$href);
	my $b = join(' ', sort keys %$format);

	return unless ($a eq $b);

	foreach my $key (keys %$href)
	{
		my $a = $href->{$key};
		my $b = $format->{$key};

		if (defined($a) && ref($a) && ref($a) =~ m/HASH/)
		{
			if (defined($b) && ref($b) && ref($b) =~ m/HASH/)
			{
				# Recurse on this field
				return unless hash_as_format($a, $b);
			}
		}
		return if (defined($b) && ref($b) && ref($b) =~ m/HASH/);
	}
	return 1;
}

# Called by new() when a hash is tied
sub _init
{
	my ($self, %params) = @_;
	my $classname = ref($self) || "HeapTuple";

	my $fh = $params{fh};
	my ($lp_len, $lp_off) =
		map {
			croak "missing required parameter '$_' in HeapTuple::TIE"
				unless exists $params{$_};
			croak "parameter '$_' must be defined in HeapTuple::TIE"
				unless defined $params{$_};
			croak "parameter '$_' must be a non-negative integer " .
				  "in HeapTuple::TIE"
				unless $params{$_} =~ m/^\d+$/;
			$params{$_}
		} qw(lp_len lp_off);

	croak "tuple lp_len too short for tuple header: lp_len:$lp_len " .
		 "in HeapTuple::TIE"
		if ($lp_len < HEAPTUPLEHEADER_PACK_LENGTH);
	croak "lp_off out of bounds: $lp_off in HeapTuple::TIE"
		if ($lp_off < 0);

	# On-disk tuples get tied with a filehandle that is already
	# opened and seek'ed to the right location.  The caller gives
	# us an lp_len so we know how much to read, and an lp_off
	# that we just store for later.
	if (defined $params{fh})
	{
		$self->{lp_len} = $lp_len;
		$self->{lp_off} = $lp_off;
		$self->readfh($params{fh}, $params{lp_len});
		return $self;
	}

	# In-memory tuples do not get tied with a filehandle, but they
	# still have an lp_len and lp_off for us to track.  We mock up
	# a zero heap tuple of the given length.  We use HeapTuple::read
    # to do the initialization, so that we don't get subtly behavior
    # than what read() would do.
	my $packstr = "C[$lp_len]";
	my @packarray = map { 0 } (1..$lp_len);
	my $ZEROS = pack($packstr, @packarray);
	$self->read($ZEROS, $ZEROS, 0);
	return $self;
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

# Wrapper around our read() method, below, for callers who have an open
# filehandle for reading.
sub readfh {
	my ($self, $fh, $lp_len) = @_;

	# read the header
	my $header;
	my $result = sysread($fh, $header, HEAPTUPLEHEADER_PACK_LENGTH);
	if ($result >= 0 && $result < HEAPTUPLEHEADER_PACK_LENGTH)
	{
		warn("Read partial tuple header: Expected " .
				HEAPTUPLEHEADER_PACK_LENGTH .
				" bytes, but got $result");
	}
	elsif (! defined $result)
	{
		confess "Cannot read tuple header from file: $!";
	}

	# read the body
	my $body;
	my $bodylen = $lp_len - HEAPTUPLEHEADER_PACK_LENGTH;
	$result = sysread($fh, $body, $bodylen);
	if ($result >= 0 && $result < $bodylen)
	{
		warn("Read partial tuple body: Expected $bodylen bytes, " .
			 "but got $result");
	}
	elsif (! defined $result)
	{
		confess "Cannot read tuple body from file: $!";
	}
	$self->read($header, $body, $bodylen);
}

# Reads the data for a tuple from a packed string, such as would come from
# reading a HeapTuple directly from a heap file, but no assumptions about the
# origin of the packed data are made.
sub read {
	my ($self, $header, $body, $bodylen) = @_;

	# Purge any pre-existing data
	delete $self->{tuple};

	# Save the packed data for output
	$self->{packed_header} = $header;
	$self->{packed_body} = $body;
	$self->{packed_bodylen} = $bodylen;

	# Read the HeapTupleHeader
	my @unpacked = unpack(HEAPTUPLEHEADER_PACK_CODE, $header);
	my ($t_xmin, $t_xmax, $t_field3, $bi_hi, $bi_lo,
		$ip_posid, $t_infomask2, $t_infomask, $t_hoff) = @unpacked;

	# Interpret the packed data as a HeapTuple, not a DatumTuple
	$self->{tuple} = {
		t_choice => {
			t_heap => {
				t_xmin => $t_xmin,
				t_xmax => $t_xmax,
				t_field3 => $t_field3,
			},
		},
		t_ctid => {
			ip_blkid => {
				bi_hi => $bi_hi,
				bi_lo => $bi_lo,
			},
			ip_posid => $ip_posid,
		},
		t_infomask2 => $t_infomask2,
		t_infomask => $t_infomask,
		t_hoff => $t_hoff,
	};
}

# Pack the header into the binary file format appropriate for writing back to
# the heap file.
sub repack {
	my ($self) = @_;
	$self->{packed_header} =
		pack(HEAPTUPLEHEADER_PACK_CODE,
				$self->{tuple}->{t_choice}->{t_heap}->{t_xmin},
				$self->{tuple}->{t_choice}->{t_heap}->{t_xmax},
				$self->{tuple}->{t_choice}->{t_heap}->{t_field3},
				$self->{tuple}->{t_ctid}->{ip_blkid}->{bi_hi},
				$self->{tuple}->{t_ctid}->{ip_blkid}->{bi_lo},
				$self->{tuple}->{t_ctid}->{ip_posid},
				$self->{tuple}->{t_infomask2},
				$self->{tuple}->{t_infomask},
				$self->{tuple}->{t_hoff});
}

# Write our packed data to an open filehandle provided by the caller.  The
# caller is responsible for seeking to the beginning of the page on which we are
# to write the data, and we are responsible for writting it to the appropriate
# offset into that page.
sub writefh
{
	my ($self, $fh) = @_;
	my $lp_len = $self->{lp_len};
	my $lp_off = $self->{lp_off};
	$self->repack();

	# Sanity check
	my $packsize = $self->{packed_bodylen} + HEAPTUPLEHEADER_PACK_LENGTH;
	confess "Attempt to write tuple of size $packsize when $lp_len expected"
		unless ($packsize == $lp_len);

	# The caller should have seek'ed to the beginning of the page,
	# but it's our job to seek to our lp_off within the page
	my $startpos = sysseek($fh, $lp_off, SEEK_CUR);

	my $written = syswrite($fh, $self->{packed_header}, HEAPTUPLEHEADER_PACK_LENGTH);
	confess "syswrite failed while writing tuple header: $!"
		unless defined $written;
	confess "syswrite wrote fewer tuple header bytes than expected"
		unless ($written == HEAPTUPLEHEADER_PACK_LENGTH);
	my $newpos = sysseek($fh, 0, SEEK_CUR);
	confess "syswrite did not advance fh as expected for header"
		unless ($newpos = $startpos + HEAPTUPLEHEADER_PACK_LENGTH);

	$written = syswrite($fh, $self->{packed_body}, $self->{packed_bodylen});
	confess "syswrite failed while writing tuple body: $!"
		unless defined $written;
	confess "syswrite wrote fewer tuple body bytes than expected"
		unless ($written == $self->{packed_bodylen});
	$newpos = sysseek($fh, 0, SEEK_CUR);
	confess "syswrite did not advance fh as expected for body"
		unless ($newpos = $startpos + $lp_len);
}

#
# Public methos provided through the "tie" interface begin here
#

=head1 METHODS

=over

=item TIEHASH classname

The method invoked by the command "tie %hash, 'HeapTuple'".

Don't do this yourself.  Use HeapPage tied hashes, or better yet,
HeapFile tied arrays.  Tying a HeapTuple directly is difficult,
error prone, and uses an interface that may change in the future.

=cut

sub TIEHASH
{
	my $classname = shift;
	my $self = $classname->new(@_);
}

=pod

=item UNTIE this

The method invoked by the command "untie %hash".

Don't do this yourself.  HeapTuples belong to the tied HeapPage.

=cut

sub UNTIE
{
	my ($self) = @_;
}

=item STORE this, key, value

The method invoked when a field in the tied hash is written or modified.

Checks the key against the list of valid tuple field names and the value
against the supported range of the field, raising exceptions for invalid
key/value pairs.  For valid pairs, the hash is updated.

There are many "magical" keys supported.  See FETCH for details.

=cut

sub STORE
{
	my ($self, $key, $value) = @_;

	if ($key eq 't_choice')
	{
		if (hash_has_format(
					$value, {
						t_heap => {
							t_xmin => [MIN_UINT32, MAX_UINT32],
							t_xmax => [MIN_UINT32, MAX_UINT32],
							t_field3 => [MIN_UINT32, MAX_UINT32],
						},
					}) ||
			hash_has_format(
					$value, {
						t_datum => {
							datum_len_ => [MIN_UINT32, MAX_UINT32],
							datum_typmod => [MIN_UINT32, MAX_UINT32],
							datum_typeid => [MIN_UINT32, MAX_UINT32],
						}
					}))
		{
			$self->{tuple}->{t_choice} = $value;
		}
		else
		{
			my $valdump = Dumper($value);
			confess "t_choice value wrong format or values out of bounds: $valdump";
		}
	}
	elsif ($key eq 't_ctid')
	{
		if (hash_has_format(
					$value, {
						ip_blkid => {
							bi_hi => [MIN_UINT16, MAX_UINT16],
							bi_lo => [MIN_UINT16, MAX_UINT16],
						},
						ip_posid => [MIN_UINT16, MAX_UINT16],
					}))
		{
			$self->{tuple}->{t_ctid} = $value;
		}
		else
		{
			my $valdump = Dumper($value);
			confess "t_ctid value wrong format or values out of bounds: $valdump";
		}
	}
	elsif ($key eq 't_infomask2')
	{
		confess "t_infomask2 value $value out of bounds"
			unless(integer_in_range($value, MIN_UINT16, MAX_UINT16));
		$self->{tuple}->{t_infomask2} = $value;
	}
	elsif ($key eq 't_infomask')
	{
		confess "t_infomask value $value out of bounds"
			unless(integer_in_range($value, MIN_UINT16, MAX_UINT16));
		$self->{tuple}->{t_infomask} = $value;
	}
	elsif ($key eq 't_hoff')
	{
		confess "t_hoff value $value out of bounds"
			unless(integer_in_range($value, MIN_UINT8, MAX_UINT8));
		$self->{tuple}->{t_hoff} = $value;
	}
	elsif ($key eq 't_bits')
	{
		croak "Not yet implemented: Cannot overwrite t_bits in HeapTuple";
	}

	# Allow setting/clearing t_infomask bit fields by name
	elsif ($key eq 'HEAP_HASNULL')
	{
		$self->{tuple}->{t_infomask} =
			($self->{tuple}->{t_infomask} & (~HEAP_HASNULL)) |
			($value ? HEAP_HASNULL : 0);
	}
	elsif ($key eq 'HEAP_HASVARWIDTH')
	{
		$self->{tuple}->{t_infomask} =
			($self->{tuple}->{t_infomask} & (~HEAP_HASVARWIDTH)) |
			($value ? HEAP_HASVARWIDTH : 0);
	}
	elsif ($key eq 'HEAP_HASEXTERNAL')
	{
		$self->{tuple}->{t_infomask} =
			($self->{tuple}->{t_infomask} & (~HEAP_HASEXTERNAL)) |
			($value ? HEAP_HASEXTERNAL : 0);
	}
	elsif ($key eq 'HEAP_HASOID_OLD')
	{
		$self->{tuple}->{t_infomask} =
			($self->{tuple}->{t_infomask} & (~HEAP_HASOID_OLD)) |
			($value ? HEAP_HASOID_OLD : 0);
	}
	elsif ($key eq 'HEAP_XMAX_KEYSHR_LOCK')
	{
		$self->{tuple}->{t_infomask} =
			($self->{tuple}->{t_infomask} & (~HEAP_XMAX_KEYSHR_LOCK)) |
			($value ? HEAP_XMAX_KEYSHR_LOCK : 0);
	}
	elsif ($key eq 'HEAP_COMBOCID')
	{
		$self->{tuple}->{t_infomask} =
			($self->{tuple}->{t_infomask} & (~HEAP_COMBOCID)) |
			($value ? HEAP_COMBOCID : 0);
	}
	elsif ($key eq 'HEAP_XMAX_EXCL_LOCK')
	{
		$self->{tuple}->{t_infomask} =
			($self->{tuple}->{t_infomask} & (~HEAP_XMAX_EXCL_LOCK)) |
			($value ? HEAP_XMAX_EXCL_LOCK : 0);
	}
	elsif ($key eq 'HEAP_XMAX_LOCK_ONLY')
	{
		$self->{tuple}->{t_infomask} =
			($self->{tuple}->{t_infomask} & (~HEAP_XMAX_LOCK_ONLY)) |
			($value ? HEAP_XMAX_LOCK_ONLY : 0);
	}
	elsif ($key eq 'HEAP_XMIN_COMMITTED')
	{
		$self->{tuple}->{t_infomask} =
			($self->{tuple}->{t_infomask} & (~HEAP_XMIN_COMMITTED)) |
			($value ? HEAP_XMIN_COMMITTED : 0);
	}
	elsif ($key eq 'HEAP_XMIN_INVALID')
	{
		$self->{tuple}->{t_infomask} =
			($self->{tuple}->{t_infomask} & (~HEAP_XMIN_INVALID)) |
			($value ? HEAP_XMIN_INVALID : 0);
	}
	elsif ($key eq 'HEAP_XMAX_COMMITTED')
	{
		$self->{tuple}->{t_infomask} =
			($self->{tuple}->{t_infomask} & (~HEAP_XMAX_COMMITTED)) |
			($value ? HEAP_XMAX_COMMITTED : 0);
	}
	elsif ($key eq 'HEAP_XMAX_INVALID')
	{
		$self->{tuple}->{t_infomask} =
			($self->{tuple}->{t_infomask} & (~HEAP_XMAX_INVALID)) |
			($value ? HEAP_XMAX_INVALID : 0);
	}
	elsif ($key eq 'HEAP_XMAX_IS_MULTI')
	{
		$self->{tuple}->{t_infomask} =
			($self->{tuple}->{t_infomask} & (~HEAP_XMAX_IS_MULTI)) |
			($value ? HEAP_XMAX_IS_MULTI : 0);
	}
	elsif ($key eq 'HEAP_UPDATED')
	{
		$self->{tuple}->{t_infomask} =
			($self->{tuple}->{t_infomask} & (~HEAP_UPDATED)) |
			($value ? HEAP_UPDATED : 0);
	}
	elsif ($key eq 'HEAP_MOVED_OFF')
	{
		$self->{tuple}->{t_infomask} =
			($self->{tuple}->{t_infomask} & (~HEAP_MOVED_OFF)) |
			($value ? HEAP_MOVED_OFF : 0);
	}
	elsif ($key eq 'HEAP_MOVED_IN')
	{
		$self->{tuple}->{t_infomask} =
			($self->{tuple}->{t_infomask} & (~HEAP_MOVED_IN)) |
			($value ? HEAP_MOVED_IN : 0);
	}

	# Allow storing t_infomask2 bit fields by name
	elsif ($key eq 'HEAP_NATTS_MASK')
	{
		$self->{tuple}->{t_infomask2} =
			($self->{tuple}->{t_infomask2} & (~HEAP_NATTS_MASK)) |
			($value & HEAP_NATTS_MASK);
	}
	elsif ($key eq 'HEAP_KEYS_UPDATED')
	{
		$self->{tuple}->{t_infomask2} =
			($self->{tuple}->{t_infomask2} & (~HEAP_KEYS_UPDATED)) |
			($value ? HEAP_KEYS_UPDATED : 0);
	}
	elsif ($key eq 'HEAP_HOT_UPDATED')
	{
		$self->{tuple}->{t_infomask2} =
			($self->{tuple}->{t_infomask2} & (~HEAP_HOT_UPDATED)) |
			($value ? HEAP_HOT_UPDATED : 0);
	}
	elsif ($key eq 'HEAP_ONLY_TUPLE')
	{
		$self->{tuple}->{t_infomask2} =
			($self->{tuple}->{t_infomask2} & (~HEAP_ONLY_TUPLE)) |
			($value ? HEAP_ONLY_TUPLE : 0);
	}

	# Allow storing leaf fields by name
	elsif ($key eq 't_xmin')
	{
		($self->{tuple}->{t_choice}->{t_heap}->{t_xmin} = $value & 0xFFFFFFFF);
	}
	elsif ($key eq 't_xmax')
	{
		($self->{tuple}->{t_choice}->{t_heap}->{t_xmax} = $value & 0xFFFFFFFF);
	}
	elsif ($key eq 't_field3')
	{
		($self->{tuple}->{t_choice}->{t_heap}->{t_field3} = $value & 0xFFFFFFFF);
	}
	elsif ($key eq 'bi_hi')
	{
		($self->{tuple}->{t_ctid}->{ip_blkid}->{bi_hi} = $value & 0xFFFF);
	}
	elsif ($key eq 'bi_lo')
	{
		($self->{tuple}->{t_ctid}->{ip_blkid}->{bi_lo} = $value & 0xFFFF);
	}
	elsif ($key eq 'ip_posid')
	{
		($self->{tuple}->{t_ctid}->{ip_posid} = $value & 0xFFFF);
	}
	elsif ($key eq 'NULL_BITFIELD')
	{
		croak "Not yet implemented: Cannot overwrite NULL_BITFIELD in HeapTuple";
	}
	elsif ($key eq 'NULL_NIBBLEFIELD')
	{
		croak "Not yet implemented: Cannot overwrite NULL_NIBBLEFIELD in HeapTuple";
	}
	elsif ($key eq 'OID_OLD')
	{
		croak "Not yet implemented: Cannot overwrite OID_OLD in HeapTuple";
	}

	else
	{
		confess "Unrecognized heap tuple key: $key";
	}
}

=pod

=item FETCH this, key

The method invoked when a field in the tied hash is read.

Checks the key against the list of valid tuple field names and raises an
exception for unrecognized fields.  For valid fields, returns the value last
associated with the field name.

Valid keys are those returned by keys(), but also a number of "magical"
keys for direct access to subfields and even bitmasks within those subfields.

The "magical" bit fields take and return boolean values, not bits.

"Magical" bit fields indexing into t_infomask:

=over 4

=item HEAP_HASNULL

=item HEAP_HASVARWIDTH

=item HEAP_HASEXTERNAL

=item HEAP_HASOID_OLD

=item HEAP_XMAX_KEYSHR_LOCK

=item HEAP_COMBOCID

=item HEAP_XMAX_EXCL_LOCK

=item HEAP_XMAX_LOCK_ONLY

=item HEAP_XMIN_COMMITTED

=item HEAP_XMIN_INVALID

=item HEAP_XMAX_COMMITTED

=item HEAP_XMAX_INVALID

=item HEAP_XMAX_IS_MULTI

=item HEAP_UPDATED

=item HEAP_MOVED_OFF

=item HEAP_MOVED_IN

=back

"Magical" bit fields indexing into t_infomask2:

=over 4

=item HEAP_KEYS_UPDATED

=item HEAP_HOT_UPDATED

=item HEAP_ONLY_TUPLE

=back

"Magical" multibit field indexing into t_infomask2:

=over 4

=item HEAP_NATTS_MASK

The number of attributes, not a boolean but a number.

=back

Other "magical" fields:

=over 4

=item t_xmin

=item t_xmax

=item t_field3

=item bi_hi

=item bi_lo

=item ip_posid

=item t_hoff

=item OID_OLD

If HEAP_HASOID_OLD is true, returns the Oid.  This should only
be true for rows written by an older version of postgres.

=item NULL_BITFIELD

The t_bits field expressed as a bit field, like "1110011".
Note that zeros in the field represent nulls, and ones represent
not-null values.  The ordering of the bits in the field matches
the ordering of the attributes.

=item NULL_NIBBLEFIELD

The t_bits field expressed in hexadecimal nibbles, like "e7".
Note that the nibble field may not look quite right when compared
against the bit field, as the big-endian vs. little-endian
distinction might make the bits appear flipped around relative
to the hexadecimal nibbles.

=back

=cut

sub FETCH
{
	my ($self, $key) = @_;

	# Allow fetching tuple fields by name.  These are the only fields
	# that a caller would see if they iterated over the keys of the hash
	return $self->{tuple}->{$key}
		if ($TupleKeys{$key});

	# The rest of the keys we support are auto-magical.  These keys
	# cannot be seen through keys() or each(), but we still accept them
	# as short-cuts for the fields they name

	# Allow fetching t_infomask bit fields by name
	return (($self->{tuple}->{t_infomask} & HEAP_HASNULL) ? 1 : 0)
		if ($key eq 'HEAP_HASNULL');
	return (($self->{tuple}->{t_infomask} & HEAP_HASVARWIDTH) ? 1 : 0)
		if ($key eq 'HEAP_HASVARWIDTH');
	return (($self->{tuple}->{t_infomask} & HEAP_HASEXTERNAL) ? 1 : 0)
		if ($key eq 'HEAP_HASEXTERNAL');
	return (($self->{tuple}->{t_infomask} & HEAP_HASOID_OLD) ? 1 : 0)
		if ($key eq 'HEAP_HASOID_OLD');
	return (($self->{tuple}->{t_infomask} & HEAP_XMAX_KEYSHR_LOCK) ? 1 : 0)
		if ($key eq 'HEAP_XMAX_KEYSHR_LOCK');
	return (($self->{tuple}->{t_infomask} & HEAP_COMBOCID) ? 1 : 0)
		if ($key eq 'HEAP_COMBOCID');
	return (($self->{tuple}->{t_infomask} & HEAP_XMAX_EXCL_LOCK) ? 1 : 0)
		if ($key eq 'HEAP_XMAX_EXCL_LOCK');
	return (($self->{tuple}->{t_infomask} & HEAP_XMAX_LOCK_ONLY) ? 1 : 0)
		if ($key eq 'HEAP_XMAX_LOCK_ONLY');
	return (($self->{tuple}->{t_infomask} & HEAP_XMIN_COMMITTED) ? 1 : 0)
		if ($key eq 'HEAP_XMIN_COMMITTED');
	return (($self->{tuple}->{t_infomask} & HEAP_XMIN_INVALID) ? 1 : 0)
		if ($key eq 'HEAP_XMIN_INVALID');
	return (($self->{tuple}->{t_infomask} & HEAP_XMAX_COMMITTED) ? 1 : 0)
		if ($key eq 'HEAP_XMAX_COMMITTED');
	return (($self->{tuple}->{t_infomask} & HEAP_XMAX_INVALID) ? 1 : 0)
		if ($key eq 'HEAP_XMAX_INVALID');
	return (($self->{tuple}->{t_infomask} & HEAP_XMAX_IS_MULTI) ? 1 : 0)
		if ($key eq 'HEAP_XMAX_IS_MULTI');
	return (($self->{tuple}->{t_infomask} & HEAP_UPDATED) ? 1 : 0)
		if ($key eq 'HEAP_UPDATED');
	return (($self->{tuple}->{t_infomask} & HEAP_MOVED_OFF) ? 1 : 0)
		if ($key eq 'HEAP_MOVED_OFF');
	return (($self->{tuple}->{t_infomask} & HEAP_MOVED_IN) ? 1 : 0)
		if ($key eq 'HEAP_MOVED_IN');

	# Allow fetching t_infomask2 bit fields by name
	return $self->{tuple}->{t_infomask2} & HEAP_NATTS_MASK
		if ($key eq 'HEAP_NATTS_MASK');
	return (($self->{tuple}->{t_infomask2} & HEAP_KEYS_UPDATED) ? 1 : 0)
		if ($key eq 'HEAP_KEYS_UPDATED');
	return (($self->{tuple}->{t_infomask2} & HEAP_HOT_UPDATED) ? 1 : 0)
		if ($key eq 'HEAP_HOT_UPDATED');
	return (($self->{tuple}->{t_infomask2} & HEAP_ONLY_TUPLE) ? 1 : 0)
		if ($key eq 'HEAP_ONLY_TUPLE');

	# Allow fetching leaf fields by name
	return $self->{tuple}->{t_choice}->{t_heap}->{t_xmin}
		if ($key eq 't_xmin');
	return $self->{tuple}->{t_choice}->{t_heap}->{t_xmax}
		if ($key eq 't_xmax');
	return $self->{tuple}->{t_choice}->{t_heap}->{t_field3}
		if ($key eq 't_field3');
	return $self->{tuple}->{t_ctid}->{ip_blkid}->{bi_hi}
		if ($key eq 'bi_hi');
	return $self->{tuple}->{t_ctid}->{ip_blkid}->{bi_lo}
		if ($key eq 'bi_lo');
	return $self->{tuple}->{t_ctid}->{ip_posid}
		if ($key eq 'ip_posid');

	# Allow fetching the t_bits field as a is_notnull bit field
	if ($key eq 'NULL_BITFIELD')
	{
		return '' unless ($self->FETCH('HEAP_HASNULL'));
		my $bitlen = $self->FETCH('HEAP_NATTS_MASK');
		return unpack("b[$bitlen]", $self->{packed_body});
	}

	# Allow fetching the t_bits field as is_notnull nibbles
	if ($key eq 'NULL_NIBBLEFIELD')
	{
		return '' unless ($self->FETCH('HEAP_HASNULL'));
		my $nibblelen = POSIX::ceil($self->FETCH('HEAP_NATTS_MASK') / 4);
		return unpack("H[$nibblelen]", $self->{packed_body});
	}

	# Allow fetching the old oid
	if ($key eq 'OID_OLD')
	{
		my ($offset, $oid, $oidhex);
		return unless($self->FETCH('HEAP_HASOID_OLD'));

		# t_hoff is the offset into the tuple, not the tuple body.  We
		# need to know where to read the old oid from the packed body.
		# Subtract the header length for that, then the length of the
		# oid itself.
		$offset = $self->FETCH('t_hoff') - (HEAPTUPLEHEADER_PACK_LENGTH + 4);
		if ($offset < 0)
		{
			confess "Calculated negative offset when trying to read OID_OLD: $offset";
			return;
		}
		($_, $oid) = unpack("C[$offset]L", $self->{packed_body});
		return $oid;
	}

	# Allow fetching the body payload that comes after the t_hoff
	if ($key eq 'PAYLOAD_CHR')
	{
		my $offset = $self->FETCH('t_hoff') - HEAPTUPLEHEADER_PACK_LENGTH;
		my $bytelen = $self->{packed_bodylen} - $offset;
		my $format = "C" x $bytelen;
		my ($junk, @chr) = unpack("C[$offset]$format", $self->{packed_body});
		return @chr;
	}

	if ($key eq 'PAYLOAD_HEX')
	{
		my $offset = $self->FETCH('t_hoff') - HEAPTUPLEHEADER_PACK_LENGTH;
		my $bytelen = $self->{packed_bodylen} - $offset;
		my $format = "H2" x $bytelen;
		my ($junk, @hex) = unpack("C[$offset]$format", $self->{packed_body});
		return @hex;
	}

	# PRIVATE: Allow fetching a closure that allows the caller to serialize this
	# tuple.  This is for use by HeapPage, not end users
	if ($key eq 'SERIALIZATION_CLOSURE')
	{
		return sub {
			my $tuple = $self;
			my $fh = shift;
			return $self->writefh($fh);
		}
	}

	confess "Unrecognized heap tuple key: $key";
}

=pod

=item FIRSTKEY this

The method invoked when beginning iteration over the hash keys as a
result of calling 'keys' or 'each' on the tied hash.

=cut

sub FIRSTKEY
{
	my ($self) = @_;
	$self->{keys} = [@TupleKeys];	# Start new iteration
	return $self->NEXTKEY();
}

=pod

=item NEXTKEY this

The method invoked when continuing iteration over the hash keys as a
result of calling 'keys' or 'each' on the tied hash.

=cut

sub NEXTKEY
{
	my ($self) = @_;
	return shift(@{$self->{keys}});
}

=item EXISTS this, key

The method invoked when checking existence of a key within the tied
hash.

The set of keys which exist is invariable, because it is defined by
the format of PostgreSQL heap tuples.

=cut

sub EXISTS
{
	my ($self, $key) = @_;
	return exists $TupleKeys{$key};
}

=pod

=item DELETE this, key

The method invoked when deleting a key from a tied hash.

This method always raises an exception, as the set of keys is static
and unalterable, governed by the fixed format of PostgreSQL heap
tuples.

=cut

sub DELETE
{
	my ($self, $key) = @_;
	confess "Attempt to delete unrecognized HeapTuple key $key"
		unless exists $TupleKeys{$key};
	confess "Operation not supported: Cannot delete keys from HeapTuples: $key";
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

=item SCALAR this

The method invoked when evaluating the hash in scalar context.

Returns a string containing human readable text representing the
heap tuple fields and values.  To facilitate debugging, the returned
string shows some of the fields multiple times, under different
field names, such as packed vs. unpacked values.

=cut

sub SCALAR
{
	my ($self) = @_;
	$self->repack();

	my $template = "H[" . HEAPTUPLEHEADER_PACK_LENGTH*2 . "]";
	my @h_hex = split(//, unpack($template, $self->{packed_header}));

	confess sprintf("Do not have the expected number of hex characters: %s vs %s",
			scalar(@h_hex), 2 * HEAPTUPLEHEADER_PACK_LENGTH)
		unless (scalar(@h_hex) == 2 * HEAPTUPLEHEADER_PACK_LENGTH);

	my $null_bits = $self->FETCH('NULL_BITFIELD');
	my $null_nibbles = $self->FETCH('NULL_NIBBLEFIELD');
	my $null_padding = ' ' x (15 - length($null_nibbles));

	my $oid_old = $self->FETCH('OID_OLD');
	$oid_old = "" unless defined $oid_old;

	my @b_hex = $self->FETCH('PAYLOAD_HEX');
	my @b_chr = map { chr($_) =~ /[[:print:]]/ ? chr($_) : '.' }
					$self->FETCH('PAYLOAD_CHR');

	# Make "body" footer
	my @bodyrows = ("BODY AS HEX               ===>  PRINTABLE ASCII");
	die sprintf("programmatic error: %u characters, %u hexadecimals",
					scalar(@b_chr), scalar(@b_hex))
		 unless (scalar(@b_hex) == scalar(@b_chr));

	@b_hex = map { sprintf("%02x", $_) } @b_hex;
	while (scalar(@b_chr))
	{
		my $h = join(' ', splice(@b_hex, 0, 8));
		my $c = join(' ', splice(@b_chr, 0, 8));
		my $padding = ' ' x (24 - length($h));
		push (@bodyrows, join("$padding  ===>  ", $h, $c));
	}

	my $headerstr = sprintf(
		"%s%s %s%s %s%s %s%s            t_xmin: %u\n" .
		"%s%s %s%s %s%s %s%s            t_xmax: %u\n" .
		"%s%s %s%s %s%s %s%s          t_field3: %u\n" .
		"%s%s %s%s                   bi_hi: %u\n" .
		"%s%s %s%s                   bi_lo: %u\n" .
		"%s%s %s%s                ip_posid: %u\n" .
		"%s%s %s%s             t_infomask2: %u\n" .
		"                        Natts: %u\n" .
		"            HEAP_KEYS_UPDATED: %s\n" .
		"             HEAP_HOT_UPDATED: %s\n" .
		"              HEAP_ONLY_TUPLE: %s\n" .
		"%s%s %s%s              t_infomask: %u\n" .
		"                 HEAP_HASNULL: %s\n" .
		"             HEAP_HASVARWIDTH: %s\n" .
		"             HEAP_HASEXTERNAL: %s\n" .
		"              HEAP_HASOID_OLD: %s\n" .
		"        HEAP_XMAX_KEYSHR_LOCK: %s\n" .
		"                HEAP_COMBOCID: %s\n" .
		"          HEAP_XMAX_EXCL_LOCK: %s\n" .
		"          HEAP_XMAX_LOCK_ONLY: %s\n" .
		"          HEAP_XMIN_COMMITTED: %s\n" .
		"            HEAP_XMIN_INVALID: %s\n" .
		"          HEAP_XMAX_COMMITTED: %s\n" .
		"            HEAP_XMAX_INVALID: %s\n" .
		"           HEAP_XMAX_IS_MULTI: %s\n" .
		"                 HEAP_UPDATED: %s\n" .
		"               HEAP_MOVED_OFF: %s\n" .
		"                HEAP_MOVED_IN: %s\n" .
		"%s%s                     t_hoff: %u\n" .
		"${null_nibbles}${null_padding} NULL_BITFIELD: %s\n" .
		"                      OID_OLD: %s\n" .
		"",
		@h_hex[0..7],   $self->FETCH('t_xmin'),
		@h_hex[8..15],  $self->FETCH('t_xmax'),
		@h_hex[16..23], $self->FETCH('t_field3'),
		@h_hex[24..27], $self->FETCH('bi_hi'),
		@h_hex[28..31], $self->FETCH('bi_lo'),
		@h_hex[32..35], $self->FETCH('ip_posid'),
		@h_hex[36..39], $self->FETCH('t_infomask2'),
			$self->FETCH('HEAP_NATTS_MASK'),
			$self->FETCH('HEAP_KEYS_UPDATED'),
			$self->FETCH('HEAP_HOT_UPDATED'),
			$self->FETCH('HEAP_ONLY_TUPLE'),
		@h_hex[40..43], $self->FETCH('t_infomask'),
			$self->FETCH('HEAP_HASNULL'),
			$self->FETCH('HEAP_HASVARWIDTH'),
			$self->FETCH('HEAP_HASEXTERNAL'),
			$self->FETCH('HEAP_HASOID_OLD'),
			$self->FETCH('HEAP_XMAX_KEYSHR_LOCK'),
			$self->FETCH('HEAP_COMBOCID'),
			$self->FETCH('HEAP_XMAX_EXCL_LOCK'),
			$self->FETCH('HEAP_XMAX_LOCK_ONLY'),
			$self->FETCH('HEAP_XMIN_COMMITTED'),
			$self->FETCH('HEAP_XMIN_INVALID'),
			$self->FETCH('HEAP_XMAX_COMMITTED'),
			$self->FETCH('HEAP_XMAX_INVALID'),
			$self->FETCH('HEAP_XMAX_IS_MULTI'),
			$self->FETCH('HEAP_UPDATED'),
			$self->FETCH('HEAP_MOVED_OFF'),
			$self->FETCH('HEAP_MOVED_IN'),
		@h_hex[44..45], $self->FETCH('t_hoff'),
			$null_bits,
			$oid_old,
		);

	return join("\n", $headerstr, @bodyrows);
}

=pod

=back

=cut

1;
