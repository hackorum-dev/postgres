/*-------------------------------------------------------------------------
 *
 * explain_progressive.c
 *	  Code for the progressive explain feature
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/explain_progressive.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_progressive.h"
#include "commands/explain_state.h"
#include "foreign/fdwapi.h"
#include "funcapi.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/guc_tables.h"
#include "utils/timeout.h"


#define PROGRESSIVE_EXPLAIN_FREE_SIZE 4096

/* Global DSA handle */
static dsa_handle *progressiveExplainDSAHandle = NULL;

/* Pointer to the tracked query */
static QueryDesc *activeQueryDesc = NULL;

/* Transaction nest level of the tracked query */
static int	activeQueryXactNestLevel = -1;

/*
 * Flag set by timeout function to control when to update
 * instrumented progressive explains.
 *
 */
bool		ProgressiveExplainPending = false;

static void ProgressiveExplainPrint(QueryDesc *queryDesc);
static void ProgressiveExplainCleanup(bool isCommit);



/*
 * ProgressiveExplainSetup -
 *	  Track query descriptor and adjust instrumentation.
 *
 * If progressive explain is enabled and configured to collect
 * instrumentation details we adjust QueryDesc accordingly even
 * if the query was not initiated with EXPLAIN ANALYZE. This will
 * directly affect query execution and add computation overhead.
 */
void
ProgressiveExplainSetup(QueryDesc *queryDesc)
{
	/* Setup only if this is the outer most query */
	if (activeQueryDesc == NULL)
	{
		activeQueryDesc = queryDesc;
		activeQueryXactNestLevel = GetCurrentTransactionNestLevel();

		/*
		 * Enable instrumentation if the plan will be updated more than once.
		 */
		if (progressive_explain_interval > 0)
		{
			if (progressive_explain_timing)
				queryDesc->instrument_options |= INSTRUMENT_TIMER;
			else
				queryDesc->instrument_options |= INSTRUMENT_ROWS;
			if (progressive_explain_buffers)
				queryDesc->instrument_options |= INSTRUMENT_BUFFERS;
			if (progressive_explain_wal)
				queryDesc->instrument_options |= INSTRUMENT_WAL;
		}
	}
}

/*
 * ProgressiveExplainStart -
 *	  Responsible for initialization of all structures related to progressive
 *	  explains.
 *
 * We define a ExplainState that will be reused in every iteration of
 * plan updates.
 *
 * Progressive explain plans are updated in shared memory via global DSA
 * allocated by the first backend that runs this code.
 *
 * A periodic timeout is configured to update the plan in fixed intervals if
 * progressive explain is configured with instrumentation enabled. Otherwise
 * the plain plan is updated once.
 */
void
ProgressiveExplainStart(QueryDesc *queryDesc)
{
	ExplainState *es;

	/*
	 * Progressive explain is only done for the outer most query descriptor.
	 */
	if (queryDesc != activeQueryDesc)
		return;

	/* Initialize ExplainState to be used for all plan updates */
	es = NewExplainState();
	queryDesc->pestate = es;

	/* Local instrumentation object to be reused for every node */
	es->pe_local_instr = palloc0(sizeof(Instrumentation));

	/*
	 * Mark ExplainState as progressive so that ExplainNode() function uses a
	 * special logic when printing the plan.
	 */
	es->progressive = true;

	es->analyze = (queryDesc->instrument_options &&
				   (progressive_explain_interval > 0));
	es->buffers = (es->analyze && progressive_explain_buffers);
	es->wal = (es->analyze && progressive_explain_wal);
	es->timing = (es->analyze && progressive_explain_timing);
	es->summary = (es->analyze);
	es->format = progressive_explain_format;
	es->verbose = progressive_explain_verbose;
	es->settings = progressive_explain_settings;
	es->costs = progressive_explain_costs;

	/*
	 * We need a global exclusive lock to check that the global DSA was
	 * already created.
	 */
	LWLockAcquire(ProgressiveExplainLock, LW_EXCLUSIVE);
	if (*progressiveExplainDSAHandle == 0)
	{
		/*
		 * Create the DSA and pin it so that it persists regardless of
		 * existing backends.
		 */
		dsa_area   *a = dsa_create(LWTRANCHE_PROGRESSIVE_EXPLAIN_DSA);

		dsa_pin(a);
		*progressiveExplainDSAHandle = dsa_get_handle(a);
		dsa_detach(a);
	}
	LWLockRelease(ProgressiveExplainLock);

	/* Enable timeout only if instrumentation is enabled */
	if (es->analyze)
		enable_timeout_every(PROGRESSIVE_EXPLAIN_TIMEOUT,
							 TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
														 progressive_explain_interval),
							 progressive_explain_interval);

	/* Print progressive plan for the first time */
	ProgressiveExplainPrint(queryDesc);
}

/*
 * ProgressiveExplainUpdate
 * Updates progressive explain for instrumented runs.
 */
void
ProgressiveExplainUpdate(PlanState *node)
{
	/* Track the current PlanState */
	node->state->query_desc->pestate->pe_curr_node = node;
	ProgressiveExplainPrint(node->state->query_desc);
	node->state->query_desc->pestate->pe_curr_node = NULL;

	/* Reset timeout flag */
	ProgressiveExplainPending = false;
}

/*
 * ProgressiveExplainPrint -
 *	  Updates progressive explain in memory.
 *
 * This function resets the reusable ExplainState, updates the
 * plan and updates the DSA with new data.
 *
 * Memory allocation in the global DSA is also done here. Amount
 * of shared memory allocated depends on size of currently updated
 * plan. There may be reallocations in subsequent calls if new
 * plans don't fit in the existing area.
 */
void
ProgressiveExplainPrint(QueryDesc *queryDesc)
{
	bool		alloc_needed = false;
	QueryDesc  *currentQueryDesc = queryDesc;
	ProgressiveExplainData *pe_data;
	ExplainState *es = queryDesc->pestate;
	Size		size = 0;
	dsa_area   *a;

	/* Reset the string to be reused */
	resetStringInfo(es->str);

	/* Print the plan */
	ExplainBeginOutput(es);
	ExplainPrintPlan(es, currentQueryDesc);
	ExplainEndOutput(es);

	/*
	 * At this point we are certain that the common DSA area was already
	 * created. Just attach to it without any lock.
	 */
	a = dsa_attach(*progressiveExplainDSAHandle);

	/* Exclusive access is needed to update the data */
	LWLockAcquire(&MyProc->peLock, LW_EXCLUSIVE);

	/* Plan was never printed */
	if (!MyProc->peDSAPointer)
		alloc_needed = true;
	else
	{
		pe_data = dsa_get_address(a, MyProc->peDSAPointer);

		/*
		 * Plan does not fit in existing shared memory area. Reallocation is
		 * needed.
		 */
		if (strlen(es->str->data) > pe_data->plan_alloc_size)
		{
			dsa_free(a, MyProc->peDSAPointer);
			alloc_needed = true;
		}
	}

	if (alloc_needed)
	{
		/*
		 * The allocated size combines the length of the currently printed
		 * query plan with an additional delta defined by
		 * PROGRESSIVE_EXPLAIN_FREE_SIZE. This strategy prevents having to
		 * reallocate the segment very often, which would be needed in case
		 * the length of the next printed exceeds the previously allocated
		 * size.
		 */
		size = add_size(strlen(es->str->data),
						PROGRESSIVE_EXPLAIN_FREE_SIZE);
		MyProc->peDSAPointer = dsa_allocate(a,
											add_size(sizeof(ProgressiveExplainData), size));
		pe_data = dsa_get_address(a, MyProc->peDSAPointer);
		pe_data->plan_alloc_size = size;
	}

	/* Update shared memory with new data */
	strcpy(pe_data->plan, es->str->data);
	pe_data->last_update = GetCurrentTimestamp();

	LWLockRelease(&MyProc->peLock);

	dsa_detach(a);
}

/*
 * ProgressiveExplainFinish -
 *	  Finalizes query execution with progressive explain enabled.
 */
void
ProgressiveExplainFinish(QueryDesc *queryDesc)
{
	/*
	 * Progressive explain is only done for the outer most query descriptor.
	 */
	if (queryDesc == activeQueryDesc)
		ProgressiveExplainCleanup(true);
}

/*
 * ProgressiveExplainIsActive -
 *	  Checks if argument queryDesc is the one being tracked.
 */
bool
ProgressiveExplainIsActive(QueryDesc *queryDesc)
{
	return queryDesc == activeQueryDesc;
}

/*
 * End-of-transaction cleanup for progressive explains.
 */
void
AtEOXact_ProgressiveExplain(bool isCommit)
{
	/* Only perform cleanup if query descriptor is being tracked */
	if (activeQueryDesc != NULL)
	{
		if (isCommit)
			elog(WARNING, "leaked progressive explain query descriptor");
		ProgressiveExplainCleanup(isCommit);
	}
}

/*
 * End-of-subtransaction cleanup for progressive explains.
 */
void
AtEOSubXact_ProgressiveExplain(bool isCommit, int nestDepth)
{
	/*
	 * Only perform cleanup if progressive explain is enabled
	 * (activeQueryXactNestLevel != -1) and the transaction nested level of
	 * the aborted subtransaction is greater or equal compared to the level of
	 * the tracked query. This is to avoid doing cleanup in subtransaction
	 * aborts triggered by exception blocks in functions and procedures.
	 */
	if (activeQueryXactNestLevel >= nestDepth)
	{
		if (isCommit)
			elog(WARNING, "leaked progressive explain query descriptor");
		ProgressiveExplainCleanup(isCommit);
	}
}

/*
 * ProgressiveExplainCleanup -
 *	  Cleanup routine when progressive explain is enabled.
 *
 * This function resets values of local variables, clears
 * the DSA pointer of the local backend's PGPROC and
 * frees memory allocated in the global DSA if query
 * ended gracefully.
 */
void
ProgressiveExplainCleanup(bool isCommit)
{
	dsa_pointer p;
	dsa_area   *a;

	/* Stop timeout */
	disable_timeout(PROGRESSIVE_EXPLAIN_TIMEOUT, false);

	/* Reset timeout flag */
	ProgressiveExplainPending = false;

	/* Reset querydesc tracker and nested level */
	activeQueryDesc = NULL;
	activeQueryXactNestLevel = -1;

	/* Clear the local backend's DSA pointer */
	LWLockAcquire(&MyProc->peLock, LW_EXCLUSIVE);
	p = MyProc->peDSAPointer;
	MyProc->peDSAPointer = (dsa_pointer) NULL;
	LWLockRelease(&MyProc->peLock);

	/* Graceful execution, manually clean allocated area */
	if (isCommit)
	{
		/*
		 * At this point we are certain that the common DSA area was already
		 * created. Just attach to it without any lock.
		 */
		a = dsa_attach(*progressiveExplainDSAHandle);
		dsa_free(a, p);
		dsa_detach(a);
	}
}

/*
 * ExecProcNodeInstrExplain -
 *	  ExecProcNode wrapper that performs instrumentation calls and updates
 *	  progressive explains. By keeping this a separate function, we add
 *	  overhead only when instrumented progressive explain is enabled.
 */
TupleTableSlot *
ExecProcNodeInstrExplain(PlanState *node)
{
	TupleTableSlot *result;

	InstrStartNode(node->instrument);

	/*
	 * Update progressive after timeout is reached.
	 */
	if (ProgressiveExplainPending)
		ProgressiveExplainUpdate(node);

	result = node->ExecProcNodeReal(node);

	InstrStopNode(node->instrument, TupIsNull(result) ? 0.0 : 1.0);

	return result;
}

/*
 * ProgressiveExplainShmemSize
 * Compute shared memory space needed for shared memory
 * structures used by the progressive explain feature.
 */
Size
ProgressiveExplainShmemSize(void)
{
	Size		size = 0;

	size = add_size(size, sizeof(dsa_handle));

	return size;
}

/*
 * ProgressiveExplainShmemInit -
 *	  Initialize shared DSA handle.
 *
 * This handle will point to the global DSA area allocated
 * by the first backend that attempts to perform progressive
 * explains.
 *
 */
void
ProgressiveExplainShmemInit(void)
{
	bool		found;

	progressiveExplainDSAHandle = (dsa_handle *)
		ShmemInitStruct("Progressive Explain Data",
						sizeof(dsa_handle),
						&found);

	*progressiveExplainDSAHandle = 0;
}

/*
 * pg_stat_progress_explain -
 *	  Return the progress of progressive explains.
 */
Datum
pg_stat_progress_explain(PG_FUNCTION_ARGS)
{
#define EXPLAIN_ACTIVITY_COLS	4
	int			num_backends = pgstat_fetch_stat_numbackends();
	int			curr_backend;
	dsa_area   *a;
	ProgressiveExplainData *ped;

	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);

	/*
	 * Progressive explain DSA was not created yet so there is no progressive
	 * explain to show.
	 */
	LWLockAcquire(ProgressiveExplainLock, LW_SHARED);
	if (*progressiveExplainDSAHandle == 0)
	{
		LWLockRelease(ProgressiveExplainLock);
		return (Datum) 0;
	}
	LWLockRelease(ProgressiveExplainLock);

	/*
	 * At this point we are certain that the common DSA area was already
	 * created. Just attach to it without any lock.
	 */
	a = dsa_attach(*progressiveExplainDSAHandle);

	/* 1-based index */
	for (curr_backend = 1; curr_backend <= num_backends; curr_backend++)
	{
		Datum		values[EXPLAIN_ACTIVITY_COLS] = {0};
		bool		nulls[EXPLAIN_ACTIVITY_COLS] = {0};
		LocalPgBackendStatus *local_beentry;
		PGPROC	   *proc;
		PgBackendStatus *beentry;

		/* Get the next one in the list */
		local_beentry = pgstat_get_local_beentry_by_index(curr_backend);
		beentry = &local_beentry->backendStatus;

		proc = BackendPidGetProc(beentry->st_procpid);

		/* We are only interested processes with PGPROC */
		if (proc == NULL)
			continue;

		/*
		 * Make sure the target backend isn't updating the plan details in
		 * memory while we read it.
		 */
		LWLockAcquire(&proc->peLock, LW_SHARED);

		/*
		 * We don't look at a DSA that doesn't contain data yet, or at our own
		 * row.
		 */
		if (!DsaPointerIsValid(proc->peDSAPointer) ||
			MyProcPid == beentry->st_procpid)
		{
			LWLockRelease(&proc->peLock);
			continue;
		}

		ped = dsa_get_address(a, proc->peDSAPointer);

		/* Values available to all callers */
		if (beentry->st_databaseid != InvalidOid)
			values[0] = ObjectIdGetDatum(beentry->st_databaseid);
		else
			nulls[0] = true;

		values[1] = beentry->st_procpid;
		values[2] = TimestampTzGetDatum(ped->last_update);

		if (has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS) ||
			has_privs_of_role(GetUserId(), beentry->st_procpid))
			values[3] = CStringGetTextDatum(ped->plan);
		else
			values[3] = CStringGetTextDatum("<insufficient privilege>");

		LWLockRelease(&proc->peLock);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	dsa_detach(a);
	return (Datum) 0;
}
