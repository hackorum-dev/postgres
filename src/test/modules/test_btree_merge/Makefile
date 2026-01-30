# src/test/modules/test_btree_merge/Makefile

MODULE_big = test_btree_merge
OBJS = \
	$(WIN32RES) \
	test_btree_merge.o

PGFILEDESC = "test_btree_merge - test code for btree merge scan"

EXTENSION = test_btree_merge
DATA = test_btree_merge--1.0.sql

REGRESS = test_btree_merge

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_btree_merge
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
