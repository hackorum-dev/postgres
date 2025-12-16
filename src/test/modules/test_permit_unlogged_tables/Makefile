# src/test/modules/test_permit_unlogged_tables/Makefile

MODULE = test_permit_unlogged_tables
PGFILEDESC = "test_permit_unlogged_tables -- check permit_unlogged_tables setting is working fine"

REGRESS = test_permit_unlogged_tables
EXTRA_REGRESS_OPTS=--temp-config=$(top_builddir)/src/test/modules/test_permit_unlogged_tables/test.conf

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_permit_unlogged_tables
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
# we are setting DONT_INCLUDE_TEMP_CONFIG flag to not to include conf from contrib-global.mk
DONT_INCLUDE_TEMP_CONFIG = 1
include $(top_srcdir)/contrib/contrib-global.mk
endif
