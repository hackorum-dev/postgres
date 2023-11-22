# contrib/snowflake_sequence/Makefile

MODULE_big = snowflake_sequence
OBJS = \
	$(WIN32RES) \
	snowflake_sequence.o

EXTENSION = snowflake_sequence
DATA = snowflake_sequence--1.0.sql
PGFILEDESC = "System-wide unique value generator"

# REGRESS = snowflake_sequence

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/snowflake_sequence
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
