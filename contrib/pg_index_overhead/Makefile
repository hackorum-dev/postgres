# contrib/pg_index_overhead/Makefile

MODULE_big = pg_index_overhead
OBJS = \
	$(WIN32RES) \
	pg_index_overhead.o
PGFILEDESC = "pg_index_overhead - per-index maintenance instrumentation for EXPLAIN"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_index_overhead
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
