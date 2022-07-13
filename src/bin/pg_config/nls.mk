# src/bin/pg_config/nls.mk
CATALOG_NAME     = pg_config
GETTEXT_FILES    = $(notdir $(wildcard $(srcdir)/*.c)) \
                   ../../common/config_info.c ../../common/exec.c
