# src/test/modules/test_listsort/Makefile

MODULE_big = test_listsort
OBJS = \
	$(WIN32RES) \
	test_listsort.o
PGFILEDESC = "test_listsort - example of a custom sort for floating point numbers"

EXTENSION = test_listsort
DATA = test_listsort--1.0.sql

REGRESS = test_listsort

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_listsort
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
