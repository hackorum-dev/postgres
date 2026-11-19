# src/test/modules/test_tableam_logical/Makefile

MODULES = test_tableam_logical

EXTENSION = test_tableam_logical
DATA = test_tableam_logical--1.0.sql
PGFILEDESC = "test_tableam_logical - table access methods with and without logical decoding support"

REGRESS = repack

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_tableam_logical
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
