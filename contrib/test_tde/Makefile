# contrib/test_tde/Makefile

MODULE_big = test_tde
OBJS = \
	$(WIN32RES) \
	test_tde.o

PGFILEDESC = "test_tde - reference implementation for I/O transform hooks"

REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/test_tde/test_tde.conf
REGRESS = basic
# Disabled because these tests require "shared_preload_libraries=test_tde"
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/test_tde
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# OpenSSL is required for encryption
SHLIB_LINK += $(filter -lcrypto, $(LIBS))
