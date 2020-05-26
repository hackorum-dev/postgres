#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg
#
# Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/bin/pg/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg - consolidated PostgreSQL command line interface client"
PGAPPICON=win32

subdir = src/bin/pg
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

OBJS = \
	$(WIN32RES) \
	pg.o

all: pg

pg: $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg$(X) '$(DESTDIR)$(bindir)'/pg$(X)

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

uninstall:
	rm -f $(addprefix '$(DESTDIR)$(bindir)'/, pg$(X))

clean distclean maintainer-clean:
	rm -f pg$(X) $(OBJS) pg.o
	rm -rf tmp_check
