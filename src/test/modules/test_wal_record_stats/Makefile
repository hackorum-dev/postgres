# src/test/modules/test_wal_record_stats/Makefile

MODULE_big = test_wal_record_stats
OBJS = \
	$(WIN32RES) \
	test_wal_record_stats.o
PGFILEDESC = "test_wal_record_stats - test WAL record stats from WAL buffers"

EXTENSION = test_wal_record_stats
DATA = test_wal_record_stats--1.0.sql

TAP_TESTS = 0

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_wal_record_stats
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
