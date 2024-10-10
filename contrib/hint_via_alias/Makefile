# contrib/hint_via_alias/Makefile

MODULE_big = hint_via_alias
OBJS = \
	$(WIN32RES) \
	hint_via_alias.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/hint_via_alias
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
