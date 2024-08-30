# contrib/alphabet_join/Makefile

MODULE_big = alphabet_join
OBJS = \
	$(WIN32RES) \
	alphabet_join.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/alphabet_join
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
