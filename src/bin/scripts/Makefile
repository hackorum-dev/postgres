#-------------------------------------------------------------------------
#
# Makefile for src/bin/scripts
#
# Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/bin/scripts/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "PostgreSQL utility"
PGAPPICON=win32

subdir = src/bin/scripts
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

PROGRAMS = createdb createuser dropdb dropuser clusterdb vacuumdb reindexdb pg_isready

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
override LDFLAGS := $(libpgfeutils) $(libpq_pgport) $(LDFLAGS)

all: $(PROGRAMS)

%: %.o $(WIN32RES)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

createdb: createdb.o common.o $(stlib_pgport) $(stlib_pgfeutils) | submake-libpq
createuser: createuser.o common.o $(stlib_pgport) $(stlib_pgfeutils) | submake-libpq
dropdb: dropdb.o common.o $(stlib_pgport) $(stlib_pgfeutils) | submake-libpq
dropuser: dropuser.o common.o $(stlib_pgport) $(stlib_pgfeutils) | submake-libpq
clusterdb: clusterdb.o common.o $(stlib_pgport) $(stlib_pgfeutils) | submake-libpq
vacuumdb: vacuumdb.o common.o $(stlib_pgport) $(stlib_pgfeutils) | submake-libpq
reindexdb: reindexdb.o common.o $(stlib_pgport) $(stlib_pgfeutils) | submake-libpq
pg_isready: pg_isready.o common.o $(stlib_pgport) $(stlib_pgfeutils) | submake-libpq

install: all installdirs
	$(INSTALL_PROGRAM) createdb$(X)   '$(DESTDIR)$(bindir)'/createdb$(X)
	$(INSTALL_PROGRAM) dropdb$(X)     '$(DESTDIR)$(bindir)'/dropdb$(X)
	$(INSTALL_PROGRAM) createuser$(X) '$(DESTDIR)$(bindir)'/createuser$(X)
	$(INSTALL_PROGRAM) dropuser$(X)   '$(DESTDIR)$(bindir)'/dropuser$(X)
	$(INSTALL_PROGRAM) clusterdb$(X)  '$(DESTDIR)$(bindir)'/clusterdb$(X)
	$(INSTALL_PROGRAM) vacuumdb$(X)   '$(DESTDIR)$(bindir)'/vacuumdb$(X)
	$(INSTALL_PROGRAM) reindexdb$(X)  '$(DESTDIR)$(bindir)'/reindexdb$(X)
	$(INSTALL_PROGRAM) pg_isready$(X) '$(DESTDIR)$(bindir)'/pg_isready$(X)

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f $(addprefix '$(DESTDIR)$(bindir)'/, $(addsuffix $(X), $(PROGRAMS)))

clean distclean maintainer-clean:
	rm -f $(addsuffix $(X), $(PROGRAMS)) $(addsuffix .o, $(PROGRAMS))
	rm -f common.o $(WIN32RES)
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
