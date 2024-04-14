# src/test/modules/test_heapam/Makefile

MODULE_big = test_heapam
OBJS = \
	$(WIN32RES) \
	heappage_craft.o \
	test_heapam.o
PGFILEDESC = "test_heapam - test code for heap AM"

EXTENSION = test_heapam
DATA = test_heapam--1.0.sql

REGRESS = move_in_out pruning

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_heapam
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
