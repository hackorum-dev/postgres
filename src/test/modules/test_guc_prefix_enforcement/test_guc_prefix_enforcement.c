/*-------------------------------------------------------------------------
 *
 * test_guc_prefix_enforcement.c
 *		Test extension that properly reserves its GUC prefix.
 *
 * This extension defines a GUC variable and calls MarkGUCPrefixReserved(),
 * demonstrating correct behavior.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_guc_prefix_enforcement/test_guc_prefix_enforcement.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "utils/guc.h"

PG_MODULE_MAGIC_EXT(
	.name = "test_guc_prefix_enforcement",
	.version = PG_VERSION,
);

static char *test_var = NULL;

void
_PG_init(void)
{
	DefineCustomStringVariable("test_guc_prefix_enforcement.test_var",
							   "Test variable",
							   NULL,
							   &test_var,
							   "default",
							   PGC_SUSET,
							   0,
							   NULL,
							   NULL,
							   NULL);

	MarkGUCPrefixReserved("test_guc_prefix_enforcement");
}
