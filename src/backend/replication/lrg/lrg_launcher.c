/*-------------------------------------------------------------------------
 *
 * lrg_launcher.c
 *		  functions for lrg launcher
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/heapam.h"
#include "access/relscan.h"
#include "access/table.h"
#include "catalog/pg_database.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "replication/logicallauncher.h"
#include "replication/lrg.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

static void launch_lrg_worker(Oid dbid);
static LrgWorker* find_perdb_worker(Oid dbid);
static List* get_db_list(void);
static void scan_and_launch(void);
static void lrglauncher_worker_onexit(int code, Datum arg);

static bool ishook_registered = false;
static bool isworker_needed = false;

/*
 * Helper strcut used by get_db_list()
 */
typedef struct db_list_cell
{
	Oid dbid;
	char *dbname;
} db_list_cell;

/*
 * Launch a lrg worker related with the given database
 */
static void
launch_lrg_worker(Oid dbid)
{
	BackgroundWorker bgw;
	LrgWorker *worker = NULL;
	int slot = 0;

	LWLockAcquire(&LrgWorkerCtx->lock, LW_EXCLUSIVE);

	/*
	 * Find a free worker slot.
	 */
	for (int i = 0; i < max_logical_replication_workers; i++)
	{
		LrgWorker *pw = &LrgWorkerCtx->workers[i];

		if (pw->dbid == InvalidOid)
		{
			worker = pw;
			slot = i;
			break;
		}
	}

	/*
	 * If there are no more free worker slots, raise an ERROR now.
	 *
	 * TODO: cleanup the array at that time?
	 */
	if (worker == NULL)
	{
		LWLockRelease(&LrgWorkerCtx->lock);
		ereport(ERROR,
				errmsg("out of lrg worker slots"));
	}


	/* Prepare the worker slot. */
	worker->dbid = dbid;

	LWLockRelease(&LrgWorkerCtx->lock);

	MemSet(&bgw, 0, sizeof(BackgroundWorker));

	snprintf(bgw.bgw_name, BGW_MAXLEN, "lrg worker for database %u", dbid);
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "lrg_worker_main");
	bgw.bgw_main_arg = UInt32GetDatum(slot);

	if (!RegisterDynamicBackgroundWorker(&bgw, NULL))
	{
		/* Failed to start worker, so clean up the worker slot. */
		LWLockAcquire(&LrgWorkerCtx->lock, LW_EXCLUSIVE);
		lrg_worker_cleanup(worker);
		LWLockRelease(&LrgWorkerCtx->lock);
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("out of worker slots")));
	}
}

/*
 * Find a launched lrg worker that related with the given database.
 * This returns NUL if not exist.
 */
static LrgWorker*
find_perdb_worker(Oid dbid)
{
	int i;

	Assert(LWLockHeldByMe(&LrgWorkerCtx->lock));

	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LrgWorker *worker = &LrgWorkerCtx->workers[i];
		if (worker->dbid == dbid)
			return worker;
	}
	return NULL;
}

/*
 * Load the list of databases in this server.
 */
static List*
get_db_list()
{
	List *res = NIL;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	/* We will allocate the output data in the current memory context */
	MemoryContext resultcxt = CurrentMemoryContext;

	StartTransactionCommand();
	(void) GetTransactionSnapshot();

	rel = table_open(DatabaseRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_database dbform = (Form_pg_database) GETSTRUCT(tup);
		db_list_cell *cell;
		MemoryContext oldcxt;

		/* skip if connection is not allowed */
		if (!dbform->datallowconn)
			continue;

		/*
		 * Allocate our results in the caller's context
		 */
		oldcxt = MemoryContextSwitchTo(resultcxt);

		cell = (db_list_cell *) palloc0(sizeof(db_list_cell));
		cell->dbid = dbform->oid;
		cell->dbname = pstrdup(NameStr(dbform->datname));
		res = lappend(res, cell);

		MemoryContextSwitchTo(oldcxt);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);
	CommitTransactionCommand();

	return res;
}

/*
 * Scan pg_lrg_nodes and launch if needed.
 */
static void
scan_and_launch(void)
{
	List *list;
	ListCell   *lc;
	MemoryContext subctx;
	MemoryContext oldctx;

	subctx = AllocSetContextCreate(TopMemoryContext,
									"Lrg Launcher list",
									ALLOCSET_DEFAULT_SIZES);
	oldctx = MemoryContextSwitchTo(subctx);

	list = get_db_list();

	/*
	 * Start per-db loop
	 */
	foreach(lc, list)
	{
		db_list_cell *cell = (db_list_cell *)lfirst(lc);
		LrgWorker *worker;

		/*
		 * Have the worker already started?
		 */
		LWLockAcquire(&LrgWorkerCtx->lock, LW_EXCLUSIVE);
		worker = find_perdb_worker(cell->dbid);
		LWLockRelease(&LrgWorkerCtx->lock);

		/*
		 * If so, launcher should not do anything
		 */
		if (worker != NULL)
			continue;

		launch_lrg_worker(cell->dbid);
	}

	/* Switch back to original memory context. */
	MemoryContextSwitchTo(oldctx);
	/* Clean the temporary memory. */
	MemoryContextDelete(subctx);
}


/*
 * Callback for process exit; cleanup the controller
 */
static void
lrglauncher_worker_onexit(int code, Datum arg)
{
	LWLockAcquire(&LrgWorkerCtx->lock, LW_EXCLUSIVE);
	LrgWorkerCtx->launcher_pid = InvalidPid;
	LrgWorkerCtx->launcher_latch = NULL;
	LWLockRelease(&LrgWorkerCtx->lock);
}

/*
 * Entry point for lrg launcher
 */
void
lrg_launcher_main(Datum arg)
{
	Assert(LrgWorkerCtx->launcher_pid == 0);
	LrgWorkerCtx->launcher_pid = MyProcPid;

	/* Establish signal handlers. */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/*
	 * Register my latch to the controller
	 * for receiving notifications from lrg background worker.
	 */
	LWLockAcquire(&LrgWorkerCtx->lock, LW_EXCLUSIVE);
	LrgWorkerCtx->launcher_latch = &MyProc->procLatch;
	LrgWorkerCtx->launcher_pid = MyProcPid;
	LWLockRelease(&LrgWorkerCtx->lock);
	before_shmem_exit(lrglauncher_worker_onexit, (Datum) 0);
	ResetLatch(&MyProc->procLatch);

	/*
	 * we did not connect specific database, because launcher
	 * will read only pg_database.
	 */
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	/*
	 * main loop
	 */
	for (;;)
	{
		int rc = 0;

		CHECK_FOR_INTERRUPTS();

		/*
		 * XXX: for simplify laucnher will start a loop at fixed intervals,
		 * but it will be no-op if no one sets a latch.
		 */
#define TEMPORARY_NAP_TIME 180000L

		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   TEMPORARY_NAP_TIME, 0);
		if (rc & WL_LATCH_SET)
		{
			ResetLatch(&MyProc->procLatch);
			CHECK_FOR_INTERRUPTS();
			scan_and_launch();
		}

		/*
		 * XXX: Reload configuration file, but is it really needed for launcher?
		 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
	}
	/* Not reachable */
}

/*
 * xact callback for launcher/worker.
 */
static void
lrg_perdb_wakeup_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_COMMIT:
			if (isworker_needed)
			{
				LrgWorker *worker;
				LWLockAcquire(&LrgWorkerCtx->lock, LW_EXCLUSIVE);
				worker = find_perdb_worker(MyDatabaseId);

				/*
				 * If lrg worker related with this db has been
				 * launched, notify to the worker.
				 * If not, maybe it means that someone has called lrg_create()/lrg_node_attach(),
				 * notify to the launcher.
				 */
				if (worker != NULL)
					SetLatch(worker->worker_latch);
				else
					SetLatch(LrgWorkerCtx->launcher_latch);

				LWLockRelease(&LrgWorkerCtx->lock);
			}
			isworker_needed = false;
			break;
		/*
		 * XXX: Do we have to wake up LRG launcher and worker processes when PREPARE event?
		 */
		default:
			break;
	}
}

/*
 * Register a callback for notifying to launcher, and set a flag
 */
void
lrg_launcher_wakeup(void)
{
	if (!ishook_registered)
	{
		RegisterXactCallback(lrg_perdb_wakeup_callback, NULL);
		ishook_registered = true;
	}
	isworker_needed = true;
}
