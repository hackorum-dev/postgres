# src/test/modules/test_subxact_commit/Makefile

MODULE_big = test_subxact_commit
OBJS = \
	$(WIN32RES) \
	test_subxact_commit.o
PGFILEDESC = "test_subxact_commit - force errors at subtransaction commit"

EXTENSION = test_subxact_commit
DATA = test_subxact_commit--1.0.sql

TAP_TESTS = 1

EXTRA_INSTALL = contrib/amcheck contrib/pageinspect

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_subxact_commit
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
