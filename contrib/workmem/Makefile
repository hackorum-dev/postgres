# contrib/workmem/Makefile

MODULE_big = workmem
OBJS = \
	$(WIN32RES) \
	workmem.o
PGFILEDESC = "workmem - extension that adjusts PostgreSQL work_mem per node"

REGRESS = workmem

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/workmem
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
