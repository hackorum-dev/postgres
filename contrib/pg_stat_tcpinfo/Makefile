# contrib/pg_stat_tcpinfo/Makefile

MODULE_big = pg_stat_tcpinfo
OBJS = pg_stat_tcpinfo.o

EXTENSION = pg_stat_tcpinfo
DATA = pg_stat_tcpinfo--1.0.sql
PGFILEDESC = "pg_stat_tcpinfo - show detailed TCP connection info on Linux.'

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_stat_tcpinfo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
