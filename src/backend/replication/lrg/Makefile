#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/replication/lrg
#
# IDENTIFICATION
#    src/backend/replication/lrg/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/replication/lrg
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir) -I$(libpq_srcdir) $(CPPFLAGS)

OBJS = \
	lrg.o \
	lrg_launcher.o \
	lrg_worker.o

include $(top_srcdir)/src/backend/common.mk
