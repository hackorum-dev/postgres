MODULES = cast_jsonb_to_hstore
EXTENSION = cast_jsonb_to_hstore
DATA = cast_jsonb_to_hstore--1.0.sql
PGFILEDESC = "Convert data between different character sets"
REGRESS = cast_jsonb_to_hstore
EXTRA_INSTALL = contrib/hstore

ifdef USE_PGXS
PG_CONFIG = PG_CONFIG
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
PG_CPPFLAGS = -I$(top_srcdir)/contrib
subdir = contrib/cast_jsonb_to_hstore
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif