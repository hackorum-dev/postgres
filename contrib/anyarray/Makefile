# contrib/anyarray/Makefile

MODULE_big = anyarray
OBJS = \
	$(WIN32RES) \
	anyarray.o \
	anyarray_bool.o \
	anyarray_gin.o \
	anyarray_gist.o \
	anyarray_op.o

EXTENSION = anyarray
DATA = anyarray--1.0.sql
PGFILEDESC = "anyarray - operations and indexes for arrays of any type"

REGRESS = anyarray

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/anyarray
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
