#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/replication/lrg/libpqlrg
#
# IDENTIFICATION
#    src/backend/replication/lrg/libpqlrg/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/replication/lrg/libpqlrg
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir) -I$(libpq_srcdir) $(CPPFLAGS)

OBJS = \
	$(WIN32RES) \
	libpqlrg.o

SHLIB_LINK_INTERNAL = $(libpq)
SHLIB_LINK = $(filter -lintl, $(LIBS))
SHLIB_PREREQS = submake-libpq
PGFILEDESC = "libpqlrg"
NAME = libpqlrg

all: all-shared-lib

include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
