# src/test/modules/test_getopt_long/Makefile

PGFILEDESC = "standalone getopt_long tester"
PGAPPICON = win32

TAP_TESTS = 1

OBJS = test_getopt_long.o $(WIN32RES)

# getopt_long.o is not in OBJS, or --with-llvm would look for a
# getopt_long.c here to build getopt_long.bc from.
EXTRA_CLEAN = test_getopt_long$(X) test_getopt_long_system$(X) getopt_long.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_getopt_long
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

all: test_getopt_long$(X) test_getopt_long_system$(X)

%.o: $(top_srcdir)/$(subdir)/%.c

# The port getopt_long() is compiled in directly rather than taken from
# libpgport, which uses the system implementation where one exists.
getopt_long.o: $(top_srcdir)/src/port/getopt_long.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

test_getopt_long$(X): test_getopt_long.o getopt_long.o $(WIN32RES)
	$(CC) $(CFLAGS) $^ $(PG_LIBS_INTERNAL) $(LDFLAGS) $(LDFLAGS_EX) $(PG_LIBS) $(LIBS) -o $@

# linked against the system getopt_long(), where there is one
test_getopt_long_system$(X): test_getopt_long.o $(WIN32RES)
	$(CC) $(CFLAGS) $^ $(PG_LIBS_INTERNAL) $(LDFLAGS) $(LDFLAGS_EX) $(PG_LIBS) $(LIBS) -o $@
