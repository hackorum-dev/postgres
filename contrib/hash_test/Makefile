# contrib/hash_test/Makefile

MODULE_big = hash_test
OBJS = \
	hash_test.o

PGFILEDESC = "hash_test - demostrate hash_seq_search and transaction issue"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/hash_test
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
