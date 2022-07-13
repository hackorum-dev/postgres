# src/pl/plperl/nls.mk
CATALOG_NAME     = plperl
GETTEXT_FILES    = $(notdir $(wildcard $(srcdir)/*.c)) \
                   SPI.c Util.c
GETTEXT_TRIGGERS = $(BACKEND_COMMON_GETTEXT_TRIGGERS)
GETTEXT_FLAGS    = $(BACKEND_COMMON_GETTEXT_FLAGS)
