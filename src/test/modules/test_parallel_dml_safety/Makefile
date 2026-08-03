# src/test/modules/test_parallel_dml_safety/Makefile

MODULE_big = test_parallel_dml_safety
OBJS = \
	$(WIN32RES) \
	test_parallel_dml_safety.o
PGFILEDESC = "test_parallel_dml_safety - test code for parallel DML safety caching"

EXTENSION = test_parallel_dml_safety
DATA = test_parallel_dml_safety--1.0.sql

REGRESS = parallel_dml_safety
ISOLATION = stale_safety_cache

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_parallel_dml_safety
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
