/*--------------------------------------------------------------------------
 *
 * test_dshash.c
 *		Test dynamic shared hash tables (dshash).
 *
 * Copyright (c) 2024-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_dshash/test_dshash.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "lib/dshash.h"
#include "storage/dsm_registry.h"
#include "storage/lwlock.h"
#include "utils/dsa.h"

PG_MODULE_MAGIC;

/* Size limit for OOM tests */
#define TEST_DSHASH_SIZE_LIMIT	(128 * 1024)

/* More than enough to exhaust TEST_DSHASH_SIZE_LIMIT */
#define TEST_DSHASH_MAX_OOM_ITERATIONS	10000

typedef struct TestDshashEntry
{
	int			key;
	int			value;
}			TestDshashEntry;

/* To verify payload integrity */
#define KEY_TO_VALUE(k)		((k) ^ 0x12345678)

static const dshash_parameters test_params = {
	sizeof(int),				/* key_size */
	sizeof(TestDshashEntry),	/* entry_size */
	dshash_memcmp,
	dshash_memhash,
	dshash_memcpy,
	LWTRANCHE_FIRST_USER_DEFINED	/* tranche_id, overwritten at runtime */
};

static void
init_tranche(void *ptr, void *arg)
{
	int		   *tranche_id = (int *) ptr;

	*tranche_id = LWLockNewTrancheId("test_dshash");
}

/*
 * test_dshash_basic
 *
 * Test insert, find, sequential scan, delete_current, delete_key,
 * delete of a nonexistent key, and dshash_dump, and re-insertions
 * after deletes.
 */
PG_FUNCTION_INFO_V1(test_dshash_basic);
Datum
test_dshash_basic(PG_FUNCTION_ARGS)
{
	int		   *tranche_id;
	bool		found;
	dsa_area   *area;
	dshash_table *ht;
	dshash_parameters params = test_params;
	dshash_seq_status status;
	TestDshashEntry *entry;
	int			count = 10;
	int			delete_key;
	int			scanned;
	int			nonexistent_key;

	tranche_id = GetNamedDSMSegment("test_dshash", sizeof(int),
									init_tranche, &found, NULL);
	params.tranche_id = *tranche_id;

	area = dsa_create(*tranche_id);
	ht = dshash_create(area, &params, NULL);

	/* Insert entries with a payload. */
	for (int i = 0; i < count; i++)
	{
		entry = dshash_find_or_insert(ht, &i, &found);
		if (found)
			elog(ERROR, "unexpected duplicate key %d", i);
		entry->value = KEY_TO_VALUE(i);
		dshash_release_lock(ht, entry);
	}

	/* Verify all entries via find, checking both key and value. */
	for (int i = 0; i < count; i++)
	{
		entry = dshash_find(ht, &i, false);
		if (entry == NULL || entry->key != i || entry->value != KEY_TO_VALUE(i))
			elog(ERROR, "key %d not found or corrupted", i);
		dshash_release_lock(ht, entry);
	}

	/* Dump the hash table. */
	dshash_dump(ht);

	/* Try to delete a key that does not exist. */
	nonexistent_key = count + 1;
	found = dshash_delete_key(ht, &nonexistent_key);
	if (found)
		elog(ERROR, "delete of nonexistent key %d reported found", nonexistent_key);

	/* Verify entry count via sequential scan. */
	scanned = 0;
	dshash_seq_init(&status, ht, false);
	while ((entry = dshash_seq_next(&status)) != NULL)
		scanned++;
	dshash_seq_term(&status);

	if (scanned != count)
		elog(ERROR, "seq scan returned %d entries, expected %d", scanned, count);

	/* Delete one entry via dshash_delete_entry. */
	delete_key = 0;
	entry = dshash_find(ht, &delete_key, true);
	if (entry == NULL)
		elog(ERROR, "key %d not found for delete_entry", delete_key);
	dshash_delete_entry(ht, entry);

	/* Verify it's gone. */
	entry = dshash_find(ht, &delete_key, false);
	if (entry != NULL)
	{
		dshash_release_lock(ht, entry);
		elog(ERROR, "key %d still present after delete_entry", delete_key);
	}

	/* Delete remaining entries via delete_current. */
	dshash_seq_init(&status, ht, true);
	while ((entry = dshash_seq_next(&status)) != NULL)
		dshash_delete_current(&status);
	dshash_seq_term(&status);

	/* Verify table is empty. */
	scanned = 0;
	dshash_seq_init(&status, ht, false);
	while ((entry = dshash_seq_next(&status)) != NULL)
		scanned++;
	dshash_seq_term(&status);

	if (scanned != 0)
		elog(ERROR, "expected empty table, got %d entries", scanned);

	/* Re-insert to verify the table is reusable after being emptied. */
	for (int i = 0; i < count; i++)
	{
		entry = dshash_find_or_insert(ht, &i, &found);
		if (found)
			elog(ERROR, "unexpected duplicate key %d", i);
		entry->value = KEY_TO_VALUE(i);
		dshash_release_lock(ht, entry);
	}

	dshash_destroy(ht);
	dsa_detach(area);

	PG_RETURN_VOID();
}

/*
 * test_dshash_find_or_insert_oom_error
 *
 * First, fill the hash table using DSHASH_INSERT_NO_OOM until OOM is hit
 * and handled gracefully.  Then, insert without the flag to verify that OOM
 * raises ERROR.  This also exercises resize() along the way.
 */
PG_FUNCTION_INFO_V1(test_dshash_find_or_insert_oom_error);
Datum
test_dshash_find_or_insert_oom_error(PG_FUNCTION_ARGS)
{
	int		   *tranche_id;
	bool		found;
	dsa_area   *area;
	dshash_table *ht;
	dshash_parameters params = test_params;
	int			key = 0;

	tranche_id = GetNamedDSMSegment("test_dshash", sizeof(int),
									init_tranche, &found, NULL);
	params.tranche_id = *tranche_id;

	area = dsa_create(*tranche_id);
	dsa_set_size_limit(area, TEST_DSHASH_SIZE_LIMIT);
	ht = dshash_create(area, &params, NULL);

	/* Insert until OOM — with NO_OOM flag, returns NULL instead of ERROR. */
	for (key = 0; key < TEST_DSHASH_MAX_OOM_ITERATIONS; key++)
	{
		TestDshashEntry *entry;

		entry = dshash_find_or_insert_extended(ht, &key, &found,
											   DSHASH_INSERT_NO_OOM);
		if (entry == NULL)
			break;
		dshash_release_lock(ht, entry);
	}

	if (key >= TEST_DSHASH_MAX_OOM_ITERATIONS)
		elog(ERROR, "expected out-of-memory, but completed all %d iterations",
			 TEST_DSHASH_MAX_OOM_ITERATIONS);

	/* Insert without NO_OOM flag — this should raise ERROR. */
	for (key = 0; key < TEST_DSHASH_MAX_OOM_ITERATIONS; key++)
	{
		TestDshashEntry *entry;

		entry = dshash_find_or_insert(ht, &key, &found);
		if (entry == NULL)
			elog(ERROR, "dshash_find_or_insert returned NULL unexpectedly");
		dshash_release_lock(ht, entry);
	}

	/* Should not reach here — OOM error is expected above. */
	elog(ERROR, "expected out-of-memory, but completed all %d iterations",
		 TEST_DSHASH_MAX_OOM_ITERATIONS);

	PG_RETURN_VOID();
}
