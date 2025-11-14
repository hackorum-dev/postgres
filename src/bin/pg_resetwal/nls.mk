# src/bin/pg_resetwal/nls.mk
CATALOG_NAME     = pg_resetwal
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   pg_resetwal.c \
                   entries.h \
                   ../../common/controldata_utils.c \
                   ../../common/fe_memutils.c \
                   ../../common/file_utils.c \
                   ../../common/restricted_token.c \
                   ../../fe_utils/option_utils.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) \
				   CONTROLDATA_LINE:1
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
