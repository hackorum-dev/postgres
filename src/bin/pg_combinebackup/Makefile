#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_combinebackup
#
# Copyright (c) 1998-2019, PostgreSQL Global Development Group
#
# src/bin/pg_combinebackup/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_combinebackup - combine full backup with incremental backups"
PGAPPICON=win32

subdir = src/bin/pg_combinebackup
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils

OBJS= pg_combinebackup.o $(WIN32RES)

all: pg_combinebackup

pg_combinebackup: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_combinebackup$(X) '$(DESTDIR)$(bindir)/pg_combinebackup$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_combinebackup$(X)'

clean distclean maintainer-clean:
	rm -f pg_combinebackup$(X) $(OBJS)
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
