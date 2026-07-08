# src/test/modules/buftable_bench/Makefile

PGFILEDESC = "buftable_bench - rdtsc micro-benchmark for the buffer mapping table"

MODULE_big = buftable_bench
OBJS = \
	$(WIN32RES) \
	buftable_bench.o \
	no_optimise.o

EXTENSION = buftable_bench
DATA = buftable_bench--1.0.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/buftable_bench
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
