# src/test/modules/test_dshash/Makefile

MODULE_big = test_dshash
OBJS = \
	$(WIN32RES) \
	test_dshash.o
PGFILEDESC = "test_dshash - test code for dshash"

EXTENSION = test_dshash
DATA = test_dshash--1.0.sql

REGRESS = test_dshash

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_dshash
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
