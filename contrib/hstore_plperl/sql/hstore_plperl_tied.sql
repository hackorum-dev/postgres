CREATE EXTENSION hstore_plperl CASCADE;

CREATE FUNCTION tied_hstore() RETURNS hstore
LANGUAGE plperl TRANSFORM FOR TYPE hstore AS $$
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
	$h{a} = '1'; $h{b} = '2'; $h{c} = undef;
	return \%h;
$$;
SELECT tied_hstore();

DROP FUNCTION tied_hstore();
DROP EXTENSION hstore_plperl, hstore, plperl;
