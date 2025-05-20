
#include "postgres.h"

#include "access/xact.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "utils/hsearch.h"

#define HASH_TEST_TABLE_NELEMS 2

PG_MODULE_MAGIC;

PGDLLEXPORT void hash_test_workhorse(Datum main_arg);

static HTAB *hash_test_table;

typedef struct
{
	/* key */
	int			num_key;
} hash_test_table_entry;

void
_PG_init(void)
{
	BackgroundWorker worker;

	if (!process_shared_preload_libraries_in_progress)
		return;

	MemSet(&worker, 0, sizeof(BackgroundWorker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	sprintf(worker.bgw_library_name, "hash_test");
	sprintf(worker.bgw_function_name, "hash_test_workhorse");
	sprintf(worker.bgw_name, "hash_test proccess");

	RegisterBackgroundWorker(&worker);
}

void
hash_test_workhorse(Datum main_arg)
{
	hash_test_table_entry *h_entry;
	HASHCTL		ctl;
	HASH_SEQ_STATUS hs;

	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	ctl.keysize = sizeof(int);
	ctl.entrysize = sizeof(hash_test_table_entry);

	hash_test_table = hash_create("hash_test_table",
								  HASH_TEST_TABLE_NELEMS,
								  &ctl,
								  HASH_ELEM | HASH_BLOBS);

	/* insert elements */
	for (int i = 0; i < HASH_TEST_TABLE_NELEMS; i++)
		hash_search(hash_test_table, &i, HASH_ENTER, NULL);

	/* go through hash table */
	hash_seq_init(&hs, hash_test_table);
	while ((h_entry = hash_seq_search(&hs)) != NULL)
	{
		StartTransactionCommand();
		/* do some stuff */
		CommitTransactionCommand();
	}
}
