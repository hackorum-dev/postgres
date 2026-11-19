/*--------------------------------------------------------------------------
 *
 * test_tableam_logical.c
 *		Table AMs that reuse heap's code, with and without the
 *		relation_supports_logical_decoding callback.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_tableam_logical/test_tableam_logical.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tableam.h"
#include "fmgr.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_tableam_decode_handler);
PG_FUNCTION_INFO_V1(test_tableam_nodecode_handler);

static TableAmRoutine decode_methods;
static TableAmRoutine nodecode_methods;
static bool methods_initialized = false;

static void init_methods(void);

/* Copies of heap's routine, the second without the callback. */
static void
init_methods(void)
{
	memcpy(&decode_methods, GetHeapamTableAmRoutine(), sizeof(TableAmRoutine));

	memcpy(&nodecode_methods, GetHeapamTableAmRoutine(), sizeof(TableAmRoutine));
	nodecode_methods.relation_supports_logical_decoding = NULL;

	methods_initialized = true;
}

Datum
test_tableam_decode_handler(PG_FUNCTION_ARGS)
{
	if (!methods_initialized)
		init_methods();
	PG_RETURN_POINTER(&decode_methods);
}

Datum
test_tableam_nodecode_handler(PG_FUNCTION_ARGS)
{
	if (!methods_initialized)
		init_methods();
	PG_RETURN_POINTER(&nodecode_methods);
}
