#-------------------------------------------------------------------------
#
# Makefile for src/test/bin
#
# Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/bin/bin/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "bin - the PostgreSQL standalone code binaries for testing"
PGAPPICON=win32

subdir = src/test/bin
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# make this available to TAP test scripts
export with_readline

REFDOCDIR= $(top_srcdir)/doc/src/sgml/ref

override CPPFLAGS := -I. -I$(srcdir) -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

OBJS = \
	$(WIN32RES) \
	test_json.o

PROGRAMS = test_json

all: test_json

test_json: test_json.o $(WIN32RES) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

check:
	PATH="$(abs_top_builddir)/src/test/bin:$$PATH" $(prove_check)

clean distclean maintainer-clean:
	rm -f test_json$(X) $(OBJS)
	rm -rf tmp_check
