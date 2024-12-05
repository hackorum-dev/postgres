# contrib/url/Makefile

EXTENSION = url
MODULE_big = url
DATA = url--1.0.sql
REGRESS = url
PGFILEDESC = "url - Uniform Resource Locator data type"

url_objs = lexbor/core/array.o \
	lexbor/core/array_obj.o \
	lexbor/core/bst.o \
	lexbor/core/conv.o \
	lexbor/core/diyfp.o \
	lexbor/core/dobject.o \
	lexbor/core/dtoa.o \
	lexbor/core/mem.o \
	lexbor/core/memory.o \
	lexbor/core/mraw.o \
	lexbor/core/plog.o \
	lexbor/core/str.o \
	lexbor/core/strtod.o \
	lexbor/url/url.o

OBJS = url.o $(url_objs)

SHLIB_LINK_INTERNAL = $(ICU_LIBS)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/url
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
