/*-------------------------------------------------------------------------
 *
 * test_guc_no_reserve.c
 *		Test module that defines GUCs without calling MarkGUCPrefixReserved
 *
 * This module intentionally does NOT call MarkGUCPrefixReserved() after
 * defining its custom GUC variables.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_guc_prefix_enforcement/test_guc_no_reserve.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "utils/guc.h"

PG_MODULE_MAGIC_EXT(
	.name = "test_guc_no_reserve",
	.version = PG_VERSION,
);

static char *no_reserve_var = NULL;

void
_PG_init(void)
{
	DefineCustomStringVariable("test_guc_no_reserve.some_var",
							   "A variable without prefix reservation",
							   NULL,
							   &no_reserve_var,
							   "default",
							   PGC_SUSET,
							   0,
							   NULL,
							   NULL,
							   NULL);
}
