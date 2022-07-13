# src/bin/pg_waldump/nls.mk
CATALOG_NAME     = pg_waldump
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   $(notdir $(wildcard $(srcdir)/*.c))
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS)
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
