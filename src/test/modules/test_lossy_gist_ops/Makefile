# src/test/modules/test_lossy_gist_ops/Makefile

MODULE_big = test_lossy_gist_ops
OBJS = \
	$(WIN32RES) \
	test_lossy_gist_ops.o
PGFILEDESC = "test_lossy_gist_ops - test opclass for a lossy GiST index"

EXTENSION = test_lossy_gist_ops
DATA = test_lossy_gist_ops--1.0.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_lossy_gist_ops
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
