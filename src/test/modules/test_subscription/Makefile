# src/test/modules/test_subscription/Makefile

MODULES = testsub1 testsub2
EXTENSION = testsub1 testsub2
DATA = testsub1--1.0.sql testsub2--1.0.sql

PGFILEDESC = "test_subscription - mock extensions for subscription test"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_subscription
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
