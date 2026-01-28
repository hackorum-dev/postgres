/*-------------------------------------------------------------------------
 *
 * test_guc_no_prefix.c
 *		Test extension that defines a GUC variable without a prefix.
 *
 * This extension reserves a prefix but also defines a variable without
 * a prefix, which should be flagged by prefix enforcement.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_guc_prefix_enforcement/test_guc_no_prefix.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "utils/guc.h"

PG_MODULE_MAGIC_EXT(
	.name = "test_guc_no_prefix",
	.version = PG_VERSION,
);

static char *test_var = NULL;

void
_PG_init(void)
{
	DefineCustomStringVariable("test_guc_no_prefix_var",
							   "Test variable without prefix",
							   NULL,
							   &test_var,
							   "default",
							   PGC_SUSET,
							   0,
							   NULL,
							   NULL,
							   NULL);

	MarkGUCPrefixReserved("test_guc_no_prefix");
}
