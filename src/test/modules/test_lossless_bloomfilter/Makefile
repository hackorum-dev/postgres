# src/test/modules/test_lossless_bloomfilter/Makefile

MODULE_big = test_lossless_bloomfilter
OBJS = \
	$(WIN32RES) \
	test_lossless_bloomfilter.o
PGFILEDESC = "test_lossless_bloomfilter - test code for enhanced lossless Bloom filter"

EXTENSION = test_lossless_bloomfilter
DATA = test_lossless_bloomfilter--1.0.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_lossless_bloomfilter
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif 