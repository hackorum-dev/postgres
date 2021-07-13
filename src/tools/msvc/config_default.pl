# Configuration arguments for vcbuild.
#
# Overridden by any values set in config.pl if it exists.
#
use strict;
use warnings;

our $config = {
	asserts => 0,    # --enable-cassert

	# blocksize => 8,         # --with-blocksize, 8kB by default
	# wal_blocksize => 8,     # --with-wal-blocksize, 8kB by default
	ldap      => 1,        # --with-ldap
	extraver  => undef,    # --with-extra-version=<string>
	gss       => undef,    # --with-gssapi=<path>
	icu       => undef,    # --with-icu=<path>
	nls       => undef,    # --enable-nls=<path>
	tap_tests => undef,    # --enable-tap-tests
	tcl       => undef,    # --with-tcl=<path>
	perl      => undef,    # --with-perl=<path>
	python    => undef,    # --with-python=<path>
	openssl   => undef,    # --with-openssl=<path>
	uuid      => undef,    # --with-uuid=<path>
	xml       => undef,    # --with-libxml=<path>
	xslt      => undef,    # --with-libxslt=<path>
	iconv     => undef,    # (not in configure, path to iconv)
	zlib      => undef,     # --with-zlib=<path>
	# extra preprocessor definitions in "DEFNAME" or "DEFNAME=VALUE" form.
	# Does not accept arbitrary compiler flags. e.g.
	#     ["WINVER=0x0601", "_WIN32_WINNT=0x0601"]
	defines	  => []
};

1;
