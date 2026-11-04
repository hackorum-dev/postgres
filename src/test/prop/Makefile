# src/test/prop/Makefile
#
# Property-based tests built on libhegel.  Only reachable when configure was
# run with --with-hegel (which also requires --enable-tap-tests); see
# src/test/Makefile.
#
# Everything links into one program, pg_prop, whose first argument selects the
# area:  pg_prop <area> [options] [property ...].  The TAP driver runs each
# area; a developer debugging a failure runs one area or one property by hand
# with --iterations / --seed.  See README.

PGFILEDESC = "pg_prop - property-based tests"
PGAPPICON = win32

PROGRAM = pg_prop
OBJS = \
	$(WIN32RES) \
	pg_prop.o \
	pg_prop_sql.o \
	prop_binaryheap.o \
	prop_nbtree.o \
	sql/prop_isolation.o \
	prop_prng.o

PG_CPPFLAGS = -I$(libpq_srcdir) -I$(srcdir)/sql
PG_LIBS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)
PG_LIBS_INTERNAL += -lhegel_c

NO_INSTALL = 1
TAP_TESTS = 1

subdir = src/test/prop
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk

LDFLAGS += $(foreach dir,$(LIBRARY_DIRS),-Wl,-rpath,$(dir))

pg_prop$(X): | submake-libpgfeutils
check: pg_prop$(X)
