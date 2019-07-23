# src/test/modules/test_pg_mkdir_p/Makefile

MODULE_big = test_pg_mkdir_p
OBJS = test_pg_mkdir_p.o $(WIN32RES)
PGFILEDESC = "test_pg_mkdir_p - helper function to test concurrent call to pg_mkdir_p"

EXTENSION = test_pg_mkdir_p
DATA = test_pg_mkdir_p--1.0.sql

REGRESS = test_pg_mkdir_p

SHLIB_LINK += -L$(top_builddir)/src/port -lpgport

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_pg_mkdir_p
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
