#-------------------------------------------------------------------------
#
# Makefile for src/include
#
# 'make install' installs whole contents of src/include.
#
# src/include/Makefile
#
#-------------------------------------------------------------------------

subdir = src/include
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

PUBLIC_HEADERS := $(shell grep -v '^\#' $(srcdir)/public_headers.list | grep -v '^$$')
INTERNAL_HEADERS := $(shell grep -v '^\#' $(srcdir)/internal_headers.list | grep -v '^$$')
SERVER_HEADERS := $(shell grep -v '^\#' $(srcdir)/server_headers.list | grep -v '^$$')

all: pg_config.h pg_config_os.h


install: all installdirs
# These headers are needed by the public headers of the interfaces.
	for file in $(PUBLIC_HEADERS); do \
	  $(INSTALL_DATA) $(srcdir)/$$file '$(DESTDIR)$(includedir)'/`dirname $$file` || exit; \
	done
	$(INSTALL_DATA) pg_config.h     '$(DESTDIR)$(includedir)'
	$(INSTALL_DATA) pg_config_os.h  '$(DESTDIR)$(includedir)'
# These headers are needed by the not-so-public headers of the interfaces.
	for file in $(INTERNAL_HEADERS); do \
	  $(INSTALL_DATA) $(srcdir)/$$file '$(DESTDIR)$(includedir_internal)'/`dirname $$file` || exit; \
	done
# These headers are needed for server-side development
	$(INSTALL_DATA) pg_config.h     '$(DESTDIR)$(includedir_server)'
	$(INSTALL_DATA) pg_config_os.h  '$(DESTDIR)$(includedir_server)'
	for file in $(SERVER_HEADERS); do \
	  $(INSTALL_DATA) $(srcdir)/$$file '$(DESTDIR)$(includedir_server)'/`dirname $$file` || exit; \
	done
# Generated server headers (not in list files)
	$(INSTALL_DATA) nodes/nodetags.h '$(DESTDIR)$(includedir_server)/nodes'
	$(INSTALL_DATA) utils/errcodes.h '$(DESTDIR)$(includedir_server)/utils'
	$(INSTALL_DATA) utils/fmgroids.h '$(DESTDIR)$(includedir_server)/utils'
	$(INSTALL_DATA) utils/fmgrprotos.h '$(DESTDIR)$(includedir_server)/utils'
	for file in storage/lwlocknames.h utils/probes.h utils/wait_event_types.h; do \
	  $(INSTALL_DATA) $$file '$(DESTDIR)$(includedir_server)'/$$file || exit; \
	done
	$(MAKE) -C catalog install

SERVER_HEADER_DIRS := $(sort $(patsubst %/,%,$(dir $(SERVER_HEADERS))))

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(includedir)' $(addprefix '$(DESTDIR)$(includedir)'/, $(sort $(patsubst %/,%,$(filter-out ./,$(dir $(PUBLIC_HEADERS))))))
	$(MKDIR_P) '$(DESTDIR)$(includedir_internal)' $(addprefix '$(DESTDIR)$(includedir_internal)'/, $(sort $(patsubst %/,%,$(filter-out ./,$(dir $(INTERNAL_HEADERS))))))
	$(MKDIR_P) $(addprefix '$(DESTDIR)$(includedir_server)'/, $(SERVER_HEADER_DIRS))

uninstall:
	rm -f $(addprefix '$(DESTDIR)$(includedir)'/, $(PUBLIC_HEADERS) pg_config.h pg_config_os.h)
	rm -f $(addprefix '$(DESTDIR)$(includedir_internal)'/, $(INTERNAL_HEADERS))
# heuristic...
	rm -rf $(addprefix '$(DESTDIR)$(includedir_server)'/, $(SERVER_HEADER_DIRS) *.h)
	$(MAKE) -C catalog uninstall


clean:
	rm -f utils/fmgroids.h utils/fmgrprotos.h utils/guc_tables.inc.c utils/errcodes.h utils/header-stamp
	rm -f utils/pgstat_wait_event.c utils/wait_event_funcs_data.c
	rm -f storage/lwlocknames.h utils/probes.h utils/wait_event_types.h
	rm -f nodes/nodetags.h nodes/header-stamp
	$(MAKE) -C catalog clean

distclean: clean
	rm -f pg_config.h pg_config_os.h stamp-h
