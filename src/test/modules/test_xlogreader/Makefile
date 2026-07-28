# src/test/modules/test_xlogreader/Makefile

PGFILEDESC = "test_xlogreader - XLogReader validation tests"

PROGRAM = test_xlogreader_oversized
OBJS = $(WIN32RES) test_xlogreader_oversized.o xlogreader.o

# Frontend build of backend xlogreader.c (same pattern as pg_waldump)
override CPPFLAGS := -DFRONTEND $(CPPFLAGS)
PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS_INTERNAL += $(libpq_pgport)

NO_INSTALL = 1
TAP_TESTS = 1

EXTRA_CLEAN = xlogreader.c

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_xlogreader
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

xlogreader.c: % : $(top_srcdir)/src/backend/access/transam/%
	rm -f $@ && $(LN_S) $< .
