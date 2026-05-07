# contrib/pg_compact_test/Makefile

MODULE_big = pg_compact_test
OBJS = \
	$(WIN32RES) \
	pg_compact_test.o

EXTENSION = pg_compact_test
DATA = pg_compact_test--1.0.sql
PGFILEDESC = "pg_compact_test - exercise heap compaction primitives"

REGRESS = pg_compact_test

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_compact_test
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
