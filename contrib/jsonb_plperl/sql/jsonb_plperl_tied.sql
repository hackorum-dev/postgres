CREATE EXTENSION jsonb_plperl CASCADE;

CREATE FUNCTION tied_jsonb_hash() RETURNS jsonb
LANGUAGE plperl TRANSFORM FOR TYPE jsonb AS $$
	{
		package PLPerlTiedHash;
		sub TIEHASH  { bless {}, $_[0] }
		sub STORE    { $_[0]{$_[1]} = $_[2] }
		sub FETCH    { $_[0]{$_[1]} }
		sub FIRSTKEY { my $a = keys %{$_[0]}; each %{$_[0]} }
		sub NEXTKEY  { each %{$_[0]} }
	}
	my %h;
	tie %h, 'PLPerlTiedHash';
	$h{a} = 1; $h{b} = 'boo'; $h{c} = undef;
	return \%h;
$$;
SELECT tied_jsonb_hash();

CREATE FUNCTION tied_jsonb_array() RETURNS jsonb
LANGUAGE plperl TRANSFORM FOR TYPE jsonb AS $$
	{
		package PLPerlTiedArray;
		sub TIEARRAY  { bless [], $_[0] }
		sub STORE     { $_[0][$_[1]] = $_[2] }
		sub FETCH     { $_[0][$_[1]] }
		sub FETCHSIZE { scalar @{$_[0]} }
	}
	my @a;
	tie @a, 'PLPerlTiedArray';
	$a[0] = 1; $a[1] = 'boo';
	return \@a;
$$;
SELECT tied_jsonb_array();

DROP FUNCTION tied_jsonb_hash(), tied_jsonb_array();
DROP EXTENSION jsonb_plperl, plperl;
