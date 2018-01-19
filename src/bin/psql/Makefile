#-------------------------------------------------------------------------
#
# Makefile for src/bin/psql
#
# Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/bin/psql/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "psql - the PostgreSQL interactive terminal"
PGAPPICON=win32

subdir = src/bin/psql
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
include $(top_builddir)/src/interfaces/libpq/Makefile.libdefs

REFDOCDIR= $(top_srcdir)/doc/src/sgml/ref

override CPPFLAGS := -I. -I$(srcdir) -I$(libpq_srcdir) $(CPPFLAGS)
override LDFLAGS := -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport) $(LDFLAGS)
ifeq ($(PORTNAME),darwin)
    override LDFLAGS := -headerpad_max_install_names $(LDFLAGS)
endif

OBJS=	command.o common.o conditional.o copy.o crosstabview.o \
	describe.o help.o input.o large_obj.o mainloop.o \
	prompt.o psqlscanslash.o sql_help.o startup.o stringutils.o \
	tab-complete.o variables.o \
	$(WIN32RES)


all: psql

psql: $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

help.o: sql_help.h

sql_help.c: sql_help.h ;
sql_help.h: create_help.pl $(wildcard $(REFDOCDIR)/*.sgml)
	$(PERL) $< $(REFDOCDIR) $*

psqlscanslash.c: FLEXFLAGS = -Cfe -p -p
psqlscanslash.c: FLEX_NO_BACKUP=yes
psqlscanslash.c: FLEX_FIX_WARNING=yes

distprep: sql_help.h psqlscanslash.c

install: all installdirs
	$(INSTALL_PROGRAM) psql$(X) '$(DESTDIR)$(bindir)/psql$(X)'
    ifndef NO_TEMP_INSTALL
        ifeq ($(PORTNAME),darwin)
            # With Apple's SIP (aka rootless mode) enabled one cannot use
            # DYLD_LIBRARY_PATH to point the temp install's psql to the temp
            # install's libpq to ensure the matching library is used.
            # Therefore modify the binary's embedded shared library
            # install name to point to the temp install's libpq.
			install_name_tool -change \
               '$(libdir)/lib$(NAME).$(SO_MAJOR_VERSION)$(DLSUFFIX)' \
               '$(DESTDIR)$(libdir)/lib$(NAME).$(SO_MAJOR_VERSION)$(DLSUFFIX)' \
               '$(DESTDIR)$(bindir)/psql$(X)' 
        endif
    endif
	$(INSTALL_DATA) $(srcdir)/psqlrc.sample '$(DESTDIR)$(datadir)/psqlrc.sample'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)' '$(DESTDIR)$(datadir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/psql$(X)' '$(DESTDIR)$(datadir)/psqlrc.sample'

clean distclean:
	rm -f psql$(X) $(OBJS) lex.backup

# files removed here are supposed to be in the distribution tarball,
# so do not clean them in the clean/distclean rules
maintainer-clean: distclean
	rm -f sql_help.h sql_help.c psqlscanslash.c
