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

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/shmem.h"
#include "utils/hsearch.h"


PG_MODULE_MAGIC;

/* For testing ShmemRequestStruct */
typedef struct TestShmemData
{
	int			value;
	bool		initialized;
	int			attach_count;
} TestShmemData;

static TestShmemData *TestShmem;

static bool attached_or_initialized = false;

/* For testing ShmemRequestHash */

/*
 * XXX: This is chosen to be equal to HASH_SEGSIZE (256) so that we exercise
 * the directory expansion bug.
 */
#define TEST_HASH_NELEMS	256

typedef struct TestHashEntry
{
	int32		key;			/* hash key, must be first */
} TestHashEntry;

static HTAB *TestShmemHash;

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

	ShmemRequestStruct(.name = "test_shmem area",
					   .size = sizeof(TestShmemData),
					   .ptr = (void **) &TestShmem);

	ShmemRequestHash(.name = "test_shmem overflow hash",
					 .nelems = TEST_HASH_NELEMS,
					 .hash_info.keysize = sizeof(int32),
					 .hash_info.entrysize = sizeof(TestHashEntry),
					 .hash_flags = HASH_ELEM | HASH_BLOBS,
					 .ptr = &TestShmemHash);
}

static void
test_shmem_init(void *arg)
{
	elog(LOG, "init callback called");
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
	RegisterShmemCallbacks(&TestShmemCallbacks);
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

/*
 * Test a shared memory hash table.
 *
 * Check that the correct number of elements can be inserted with
 * HASH_ENTER_NULL.  The hash table is expected to be empty before the call.
 */
PG_FUNCTION_INFO_V1(test_shmem_hash_overflow);
Datum
test_shmem_hash_overflow(PG_FUNCTION_ARGS)
{
	int			count = 0;
	int32		key;
	bool		found;
	TestHashEntry *entry;

	/* Assume the hash to be empty before the call */
	if (hash_get_num_entries(TestShmemHash) != 0)
		elog(ERROR, "hash table is not empty");

	/* Fill up the hash table */
	for (key = 0; key < TEST_HASH_NELEMS; key++)
	{
		entry = hash_search(TestShmemHash, &key, HASH_ENTER_NULL, &found);
		if (found)
			elog(ERROR, "hash entry with key %d already exists", key);
		if (entry == NULL)
			elog(ERROR, "hash table of size %d is full after only %d insertions",
				 TEST_HASH_NELEMS, count);
		count++;
	}

	/*
	 * Try to insert one more entry.  It should now fail because the hash
	 * table is full.
	 */
	entry = hash_search(TestShmemHash, &key, HASH_ENTER_NULL, &found);
	if (found)
		elog(ERROR, "hash entry with key %d already exists", key);
	if (entry != NULL)
		elog(ERROR, "hash table of size %d had space for an extra element",
			 TEST_HASH_NELEMS);

	PG_RETURN_VOID();
}
