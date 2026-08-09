/*-------------------------------------------------------------------------
 *
 * test_shmem.c
 *		Helpers to test shmem allocation routines
 *
 * Test basic memory allocation in an extension module. One notable feature
 * that is not exercised by any other module in the repository is the
 * allocating (non-DSM) shared memory after postmaster startup.
 *
 * Copyright (c) 2020-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_shmem/test_shmem.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/injection_point.h"


PG_MODULE_MAGIC;

typedef struct TestShmemData
{
	int			value;
	bool		initialized;
	int			attach_count;
} TestShmemData;

static TestShmemData *TestShmem;

static bool attached_or_initialized = false;
static int	test_shmem_area_size = sizeof(TestShmemData);
static bool test_shmem_after_startup = false;
static int	test_shmem_extra_size = 0;
static bool test_shmem_legacy_init = false;
static char test_shmem_area_name[64] = "test_shmem area";

static void test_shmem_request(void *arg);
static void test_shmem_init(void *arg);
static void test_shmem_attach(void *arg);

static const ShmemCallbacks TestShmemCallbacks = {
	.flags = SHMEM_CALLBACKS_ALLOW_AFTER_STARTUP,
	.request_fn = test_shmem_request,
	.init_fn = test_shmem_init,
	.attach_fn = test_shmem_attach,
};

static void
test_shmem_request(void *arg)
{
	elog(LOG, "test_shmem_request callback called");

	if (test_shmem_area_size == sizeof(TestShmemData))
		strcpy(test_shmem_area_name, "test_shmem area");
	else
		snprintf(test_shmem_area_name, sizeof(test_shmem_area_name),
				 "test_shmem area %d", test_shmem_area_size);

	ShmemRequestStruct(.name = test_shmem_area_name,
					   .size = test_shmem_area_size,
					   .ptr = (void **) &TestShmem);
	if (test_shmem_extra_size > 0)
		ShmemRequestStruct(.name = "test_shmem extra area",
						   .size = test_shmem_extra_size);

	if (test_shmem_after_startup)
		INJECTION_POINT("test-shmem-request", NULL);
}

static void
test_shmem_init(void *arg)
{
	elog(LOG, "init callback called");
	if (test_shmem_after_startup)
		INJECTION_POINT("test-shmem-init", NULL);
	if (test_shmem_legacy_init)
	{
		bool		found;

		(void) ShmemInitStruct("test_shmem legacy area", 1024, &found);
	}
	if (TestShmem->initialized)
		elog(ERROR, "shmem area already initialized");
	TestShmem->initialized = true;

	if (attached_or_initialized)
		elog(ERROR, "attach or initialize already called in this process");
	attached_or_initialized = true;
}

static void
test_shmem_attach(void *arg)
{
	elog(LOG, "test_shmem_attach callback called");
	if (!TestShmem->initialized)
		elog(ERROR, "shmem area not yet initialized");
	TestShmem->attach_count++;

	if (attached_or_initialized)
		elog(ERROR, "attach or initialize already called in this process");
	attached_or_initialized = true;
}

void
_PG_init(void)
{
	elog(LOG, "test_shmem module's _PG_init called");

	DefineCustomIntVariable("test_shmem.area_size",
							"Size of the shmem area to request.",
							NULL,
							&test_shmem_area_size,
							sizeof(TestShmemData),
							sizeof(TestShmemData), INT_MAX,
							PGC_USERSET,
							GUC_UNIT_BYTE,
							NULL, NULL, NULL);
	DefineCustomIntVariable("test_shmem.extra_size",
							"Size of an additional area in the same request.",
							NULL,
							&test_shmem_extra_size,
							0, 0, INT_MAX,
							PGC_USERSET,
							GUC_UNIT_BYTE,
							NULL, NULL, NULL);
	MarkGUCPrefixReserved("test_shmem");
	RegisterShmemCallbacks(&TestShmemCallbacks);
}

PG_FUNCTION_INFO_V1(test_shmem_register);
Datum
test_shmem_register(PG_FUNCTION_ARGS)
{
	test_shmem_legacy_init = PG_NARGS() > 0 && PG_GETARG_BOOL(0);
	test_shmem_after_startup = true;
	attached_or_initialized = false;
	RegisterShmemCallbacks(&TestShmemCallbacks);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(test_shmem_area_is_null);
Datum
test_shmem_area_is_null(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(TestShmem == NULL);
}

PG_FUNCTION_INFO_V1(get_test_shmem_attach_count);
Datum
get_test_shmem_attach_count(PG_FUNCTION_ARGS)
{
	if (!attached_or_initialized)
		elog(ERROR, "shmem area not attached or initialized in this process");
	if (!TestShmem->initialized)
		elog(ERROR, "shmem area not yet initialized");
	PG_RETURN_INT32(TestShmem->attach_count);
}
