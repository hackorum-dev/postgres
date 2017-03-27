# src/backend/util/mb/Unicode/encoding_dumper/Makefile

MODULES = encoding_dumper
EXTENSION = encoding_dumper
DATA = encoding_dumper--0.1.sql
PGFILEDESC = "encoding_dumper - return encoding dump"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/encoding_dumper
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
