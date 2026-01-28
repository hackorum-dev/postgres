/*-------------------------------------------------------------------------
 *
 * test_guc_wrong_prefix.c
 *		Test module that reserves one prefix but defines GUCs under another
 *
 * This module reserves the prefix "test_guc_wrong" but defines a variable
 * under "test_guc_other".
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_guc_prefix_enforcement/test_guc_wrong_prefix.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "utils/guc.h"

PG_MODULE_MAGIC_EXT(
	.name = "test_guc_wrong_prefix",
	.version = PG_VERSION,
);

static char *wrong_prefix_var = NULL;

void
_PG_init(void)
{
	DefineCustomStringVariable("test_guc_other.some_var",
							   "A variable under a different prefix than reserved",
							   NULL,
							   &wrong_prefix_var,
							   "default",
							   PGC_SUSET,
							   0,
							   NULL,
							   NULL,
							   NULL);

	MarkGUCPrefixReserved("test_guc_wrong");
}
