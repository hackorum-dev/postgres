# src/test/modules/test_guc_prefix_enforcement/Makefile

MODULES = test_guc_prefix_enforcement test_guc_no_reserve test_guc_wrong_prefix test_guc_no_prefix
PGFILEDESC = "test_guc_prefix_enforcement - test module for GUC prefix reservation enforcement"

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_guc_prefix_enforcement
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
