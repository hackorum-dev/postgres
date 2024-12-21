# contrib/stats_history/Makefile

MODULE_big = stats_history
OBJS = \
	$(WIN32RES) \
	stats_history.o

EXTENSION = stats_history
DATA = stats_history--1.0.sql
PGFILEDESC = "stats_history - vacuum history and stats"

LDFLAGS_SL += $(filter -lm, $(LIBS))

REGRESS_OPTS = 
REGRESS = 
# Disabled because these tests require "shared_preload_libraries=pg_stat_statements",
# which typical installcheck users do not have (e.g. buildfarm clients).
NO_INSTALLCHECK = 1

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/stats_history
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
