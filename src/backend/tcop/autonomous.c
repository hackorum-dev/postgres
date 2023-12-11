/*--------------------------------------------------------------------------
 *
 * autonomous.c
 *		Implementation of autonomous transactions
 *
 * Copyright (C) 2023-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/backend/tcop/autonomous.c
 *
 *
 * This implements a C API to launch an autonomous session and run SQL queries
 * in it. The session looks much like a normal database connection, but it is
 * always to the same database, and there is no authentication needed. The
 * "backend" for that connection is a background worker. Dynamic shared memory (dsm)
 * is used to store fixed data and message queues used for communication between
 * normal backend and the autonomous session. They communicate using 2 message queues
 * over the Postgres FE/BE protocol.
 * Autonomous session's errors are sent to backend.
 * Autonomous sessions are taken from the pool, when autonomous function begins to execute.
 * Each backend lazily creates pool, pools aren't shared between backends.
 * 
 *  
 * Structure of dsm:
 * +--------------------------------------------+
 * |	table of contents (toc)					|
 * +-----------------Fixed data-----------------+
 * |	database_id;							|
 * |	authenticated_user_id;					|
 * |	current_user_id;						|
 * |	sec_context;							|
 * +-----------------shm_mqs--------------------+
 * |	backend -> autonomous session shm_mq	|
 * |	autonomous session -> backend shm_mq	|
 * +--------------------------------------------+
 *
 * WARNING: currently not all statements are fully supported for execution in autonomous sessions.
 * Only 1 autonomous session executes everything sequentially. Autonomous sessions 
 * create each other sequentially recursively if nested calls of autonomous functions.
 * If the support for the execution of a statement in an autonomous session is incomplete,
 * then more sessions can be taken from the pool.
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "access/xact.h"
#include "commands/async.h"
#include "lib/stringinfo.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqmq.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "tcop/autonomous.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/resowner.h"


/* Unique number to identify autonomous table-of-contents */ 
#define AUTONOMOUS_MAGIC				0x50674267

/* Table-of-contents constants for our dynamic shared memory segment. */
#define AUTONOMOUS_KEY_FIXED_DATA		0
#define AUTONOMOUS_KEY_GUC				1
#define AUTONOMOUS_KEY_COMMAND_QUEUE	2
#define AUTONOMOUS_KEY_RESPONSE_QUEUE	3
#define AUTONOMOUS_NKEYS				4

/* Size of shared message queues used for communication */
#define AUTONOMOUS_QUEUE_SIZE			16384

/* Pool constants 
 Under normal conditions, only 1 autonomous session does everything.
 They sequentially recursively create each other if nested calls of autonomous functions.
 If the support for the execution of a statement in an autonomous session is incomplete,
 then more sessions can be taken from the pool.
 */
static const int autonomous_pool_init_capacity = 1;
static const int autonomous_pool_max_capacity = 100;

/* 
 * Restart session by lifetime. Session contains resources: memory, caches.
 * In order to prevent infinite grow of resources, clean them using session destroying.
 */
int autonomous_session_lifetime = 0;

/* Fixed-size data passed via dynamic shared memory segment. */
typedef struct AutonomousSessionFixedData
{
	Oid database_id;
	Oid authenticated_user_id;
	Oid current_user_id;
	int sec_context;
} AutonomousSessionFixedData;

/* AutonomousResult -- query result */

/* Autonomous session handle */
struct AutonomousSession
{
	dsm_segment *seg;
	BackgroundWorkerHandle *worker_handle;
	shm_mq_handle *command_qh;
	shm_mq_handle *response_qh;
	int		transaction_status;
	TimestampTz creation_time;
	bool in_use;
};

/* Autonomous session handle when used protocol Prepare-Execute */
struct AutonomousPreparedStatement
{
	AutonomousSession *session;
	Oid		   *argtypes;
	TupleDesc	tupdesc;
};

/* Pool of autonomous sessions */
typedef struct AutonomousPool
{
	unsigned capacity;
	unsigned size;
	unsigned active;
	AutonomousSession **sessions;
} AutonomousPool;
AutonomousPool *autonomous_pool = NULL;

MemoryContext AutonomousContext = NULL; /* NOTE: move to src/backend/utils/mmgr/mcxt.c */
ResourceOwner AutonomousResourceOwner = NULL; /* NOTE: move to src/backend/utils/resowner/resowner.c */

/* 
 * Stack stores information what autonomous sessions corresponds to each subblock
 * if PL/pgSQL function contains subblocks.
 * Some subblocks may be autonomous, others not. Info about autonomicity is contained in estate structure.
 * This structure is created for each PL/pgSQL function call and passed through exec_stmt_* functions (see pl_exec.c).
 * So estate->autonomous will overwritten if several subblocks are autonomous.
 * Stack is used to restore estate->autonomous for parent block after child block is finished.
 */
slist_head AutonomousStack = SLIST_STATIC_INIT(AutonomousStack);

extern void init_row_description_buf();
extern void (*check_client_encoding_hook)(void);

static TimestampTz TimestampTzPlusMinutes(TimestampTz tz, int min);

static void CreateAutonomousResourceOwner(void);
static void BeforeReleaseAutonomousResourcesCallback(int code, Datum arg);
static void ReleaseAutonomousResourcesCallback(int code, Datum arg);
static void ReleaseAutonomousResources(bool isCommit);

static void AutonomousPoolInit(void);
static void AutonomousPoolDestroy(void);
static AutonomousSession *AutonomousSessionStart(void);
static void AutonomousSessionEnd(AutonomousSession *session);
static void AutonomousSessionEndError(AutonomousSession *session);
static void AutonomousSessionFreeResources(AutonomousSession *session);

static void shm_mq_receive_stringinfo(shm_mq_handle *qh, StringInfoData *msg);
static void autonomous_check_client_encoding_hook(void);
static TupleDesc TupleDesc_from_RowDescription(StringInfo msg);
static HeapTuple HeapTuple_from_DataRow(TupleDesc tupdesc, StringInfo msg);
static void forward_NotifyResponse(StringInfo msg);
static void rethrow_errornotice(StringInfo msg, int min_elevel);
static void invalid_protocol_message(char msgtype, int phase) pg_attribute_noreturn();


/* 
 * Add minutes to TimestampTz values
 */
static TimestampTz TimestampTzPlusMinutes(TimestampTz tz, int min)
{
	return tz + ((TimestampTz)min  * 60 * 1000 * 1000);
}

/*
 * -------------------------------------------------------------------------
 */

/*
 * Establish an AutonomousResourceOwner for the current process.
 * NOTE: move to src/backend/utils/resowner/resowner.c
 */
static void
CreateAutonomousResourceOwner(void)
{
	Assert(AutonomousResourceOwner == NULL);
	AutonomousResourceOwner = ResourceOwnerCreate(NULL, "Autonomous");
	/*
	 * Register a shmem-exit callback for cleanup of autonomous-process resource
	 * owner. (This needs to run after, e.g., ShutdownXLOG.)
	 */
	before_shmem_exit(BeforeReleaseAutonomousResourcesCallback, (Datum)0);
	on_shmem_exit(ReleaseAutonomousResourcesCallback, (Datum)0);
}

/*
 * Convenience routine to release all resources tracked in
 * AutonomousResourceOwner (but that resowner is not destroyed here).
 * Warn about leaked resources if isCommit is true.
 * NOTE: move to src/backend/utils/resowner/resowner.c
 */
static void
ReleaseAutonomousResources(bool isCommit)
{
	/*
	 * At this writing, the only thing that could actually get released is
	 * autonomous pool; but we may as well do the full release protocol.
	 */
	ResourceOwnerRelease(AutonomousResourceOwner,
						 RESOURCE_RELEASE_BEFORE_LOCKS,
						 isCommit, true);
	ResourceOwnerRelease(AutonomousResourceOwner,
						 RESOURCE_RELEASE_LOCKS,
						 isCommit, true);
	ResourceOwnerRelease(AutonomousResourceOwner,
						 RESOURCE_RELEASE_AFTER_LOCKS,
						 isCommit, true);
}

/*
 * Shmem-exit callback, frees resources.
 * Warn about leaked resources if process exit code is zero (ie normal).
 * NOTE: move to src/backend/utils/resowner/resowner.c
 */
static void
BeforeReleaseAutonomousResourcesCallback(int code, Datum arg)
{
	AutonomousPoolDestroy();
}

/*
 * NOTE: move to src/backend/utils/resowner/resowner.c
 */
static void
ReleaseAutonomousResourcesCallback(int code, Datum arg)
{
	bool		isCommit = (code == 0);
	ReleaseAutonomousResources(isCommit);
}

/*
 * -------------------------------------------------------------------------
 */

/*
 * Initialize pool of autonomous sessions
 */
static void
AutonomousPoolInit(void)
{
	CreateAutonomousResourceOwner();
	AutonomousContext = AllocSetContextCreate(TopMemoryContext,
							"AutonomousContext",
							ALLOCSET_DEFAULT_SIZES);
	autonomous_pool = (AutonomousPool *) MemoryContextAlloc(AutonomousContext, sizeof(AutonomousPool));
	autonomous_pool->capacity = autonomous_pool_init_capacity;
	autonomous_pool->size = 0;
	autonomous_pool->active = 0;
	autonomous_pool->sessions = (AutonomousSession **) MemoryContextAlloc(AutonomousContext,
													autonomous_pool->capacity * sizeof(AutonomousSession *));
}

/*
 * Pop autonomous session from stack of subblocks after child subblock is finished
 */
AutonomousSession *
AutonomousSessionPopStackSession(bool isBlockAutonomous)
{
	AutonomousSession *session = NULL;
	/* Pop PLpgSQL_stmt_block from stack */
	slist_node *node = slist_pop_head_node(&AutonomousStack);
	if (isBlockAutonomous)
	{
		/* End Autonomous session */
		slist_iter iter;
		session = slist_container(AutonomousStackNode, node, node)->session;
		AutonomousSessionRelease(session);
		slist_foreach(iter, &AutonomousStack)
		{
			session = slist_container(AutonomousStackNode, node, iter.cur)->session;
			if(session) {
				break;
			}
		}
	}
	pfree(node);
	return session;
}

/*
 * Unwind stack of autonomous sessions after of SQL-exception.
 * After that return first found autonomous session
 */
AutonomousSession *
AutonomousSessionPopStackSessionException(void *block)
{
	slist_node *node = NULL;
	void *stack_block = NULL;
	AutonomousSession *session = NULL;
	slist_iter iter;

	/* Pop PLpgSQL_stmt_blocks from stack after exception
	 * Also end corresponding Autonomous sessions
	 */
	while(!slist_is_empty(&AutonomousStack)) {
		node = slist_head_node(&AutonomousStack);
		stack_block = slist_container(AutonomousStackNode, node, node)->block;
		if(stack_block == block) {
			break;
		}
		node = slist_pop_head_node(&AutonomousStack);
		session = slist_container(AutonomousStackNode, node, node)->session;
		if(session != NULL) {
			AutonomousSessionRelease(session);
		}
		pfree(node);
	}

	/* Return first found autonomous session
	 * Needed for subblocks
	 */
	slist_foreach(iter, &AutonomousStack) {
		session = slist_container(AutonomousStackNode, node, iter.cur)->session;
		if(session) {
			return session;
		}
	}
	return NULL;
}

/*
 * Destroy pool normally
 */
static void
AutonomousPoolDestroy(void)
{
	if(!autonomous_pool) {
		return;
	}
	for(unsigned i = 0; i < autonomous_pool->size; ++i) {
		AutonomousSessionEnd(autonomous_pool->sessions[i]);
	}
	pfree(autonomous_pool->sessions);
	pfree(autonomous_pool);
	autonomous_pool = NULL;
}

/*
 * Destroy pool in case of ERROR
 */
void
AutonomousPoolDestroyError(void)
{
	if(!autonomous_pool) {
		return;
	}
	for(unsigned i = 0; i < autonomous_pool->size; ++i) {
		AutonomousSessionEndError(autonomous_pool->sessions[i]);
	}
	pfree(autonomous_pool->sessions);
	pfree(autonomous_pool);
	autonomous_pool = NULL;
}

/*
 * Get autonomous session from pool
 */
AutonomousSession *
AutonomousSessionGet(void)
{
	TimestampTz curr_time = GetCurrentTimestamp();
	if(!autonomous_pool) {
		AutonomousPoolInit();
	}

	/* destroy not used sessions by timeout */
	for(unsigned i = 0;;)
	{
		if(i >= autonomous_pool->size)
		{
			break;
		}

		if(autonomous_session_lifetime)
		{
			TimestampTz death_time = TimestampTzPlusMinutes(autonomous_pool->sessions[i]->creation_time, autonomous_session_lifetime);
			if(unlikely(death_time < curr_time) && !autonomous_pool->sessions[i]->in_use)
			{
				AutonomousSessionEnd(autonomous_pool->sessions[i]);
				autonomous_pool->sessions[i] = autonomous_pool->sessions[autonomous_pool->size - 1];
				--autonomous_pool->size;
			} else {
				++i;
			}
		}
	}

	/* return 1st not used session */
	for (unsigned i = 0; i < autonomous_pool->size; ++i)
	{
		if(!autonomous_pool->sessions[i]->in_use) {
			autonomous_pool->sessions[i]->in_use = true;
			++autonomous_pool->active;
			return autonomous_pool->sessions[i];
		}
	}

	/* no free sessions. Need to start new or resize a pool at first */
	if(unlikely(autonomous_pool->size == autonomous_pool_max_capacity))
	{
		elog(ERROR, "No free autonomous sessions. Max capacity=%d of pool is reached", autonomous_pool_max_capacity);
	}

	/* resize pool if it's full */
	if (autonomous_pool->size == autonomous_pool->capacity)
	{
		unsigned new_capacity = autonomous_pool->capacity * 2;
		if(new_capacity > autonomous_pool_max_capacity)
			new_capacity = autonomous_pool_max_capacity;
		autonomous_pool->capacity = new_capacity;
		autonomous_pool->sessions = (AutonomousSession **)
			repalloc(autonomous_pool->sessions, autonomous_pool->capacity * sizeof(AutonomousSession *));
		if(!autonomous_pool->sessions) {
			elog(ERROR, "Autonomous pool can't resize");
		}
		elog(NOTICE, "Autonomous pool was resized, new capacity is %u", autonomous_pool->capacity);
	}

	/* create new session */
	{
		unsigned i = autonomous_pool->size++;
		autonomous_pool->sessions[i] = AutonomousSessionStart();
		++autonomous_pool->active;
		autonomous_pool->sessions[i]->in_use = true;
		autonomous_pool->sessions[i]->creation_time = GetCurrentTimestamp();
		return autonomous_pool->sessions[i];
	}
}

/*
 * Return autonomous session to pool
 */
void
AutonomousSessionRelease(AutonomousSession *session)
{
	if(!session) {
		elog(ERROR, "Autonomous session is NULL");
	}
	if (session->in_use) {
		session->in_use = false;
		--autonomous_pool->active;
	}
}

/*
 * Start autonomous session: allocate memory for dsm, shm_mq, start background worker, etc.
 * and return a handle. Launches background worker
 */
static AutonomousSession *
AutonomousSessionStart(void)
{
	BackgroundWorker worker;
	pid_t		pid;
	AutonomousSession *session;
	shm_toc_estimator e;
	Size		segsize;
	Size		guc_len;
	char	   *gucstate;
	dsm_segment *seg;
	shm_toc	   *toc;
	AutonomousSessionFixedData *fdata;
	shm_mq	   *command_mq;
	shm_mq	   *response_mq;
	BgwHandleStatus bgwstatus;
	StringInfoData msg;
	char		msgtype;
	MemoryContext	save_ctx;
	ResourceOwner	save_owner;

	shm_toc_initialize_estimator(&e);
	shm_toc_estimate_chunk(&e, sizeof(AutonomousSessionFixedData));
	shm_toc_estimate_chunk(&e, AUTONOMOUS_QUEUE_SIZE);
	shm_toc_estimate_chunk(&e, AUTONOMOUS_QUEUE_SIZE);
	guc_len = EstimateGUCStateSpace();
	shm_toc_estimate_chunk(&e, guc_len);
	shm_toc_estimate_keys(&e, AUTONOMOUS_NKEYS);
	segsize = shm_toc_estimate(&e);

	save_ctx = MemoryContextSwitchTo(AutonomousContext); /* XXX move to func start? */
	session = palloc(sizeof(AutonomousSession));
	session->in_use = false;
	save_owner = CurrentResourceOwner;
	CurrentResourceOwner = AutonomousResourceOwner;
	seg = dsm_create(segsize, 0);
	CurrentResourceOwner = save_owner;
	session->seg = seg;

	toc = shm_toc_create(AUTONOMOUS_MAGIC, dsm_segment_address(seg), segsize);

	/* Store fixed-size data in dynamic shared memory. */
	fdata = shm_toc_allocate(toc, sizeof(*fdata));
	fdata->database_id = MyDatabaseId;
	fdata->authenticated_user_id = GetAuthenticatedUserId();
	GetUserIdAndSecContext(&fdata->current_user_id, &fdata->sec_context);
	shm_toc_insert(toc, AUTONOMOUS_KEY_FIXED_DATA, fdata);

	/* Store GUC state in dynamic shared memory. */
	gucstate = shm_toc_allocate(toc, guc_len);
	SerializeGUCState(guc_len, gucstate);
	shm_toc_insert(toc, AUTONOMOUS_KEY_GUC, gucstate);

	command_mq = shm_mq_create(shm_toc_allocate(toc, AUTONOMOUS_QUEUE_SIZE),
							   AUTONOMOUS_QUEUE_SIZE);
	shm_toc_insert(toc, AUTONOMOUS_KEY_COMMAND_QUEUE, command_mq);
	shm_mq_set_sender(command_mq, MyProc);

	response_mq = shm_mq_create(shm_toc_allocate(toc, AUTONOMOUS_QUEUE_SIZE),
								AUTONOMOUS_QUEUE_SIZE);
	shm_toc_insert(toc, AUTONOMOUS_KEY_RESPONSE_QUEUE, response_mq);
	shm_mq_set_receiver(response_mq, MyProc);

	session->command_qh = shm_mq_attach(command_mq, seg, NULL);
	session->response_qh = shm_mq_attach(response_mq, seg, NULL);

	MemoryContextSwitchTo(save_ctx);

	worker.bgw_flags =
		BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	sprintf(worker.bgw_library_name, "postgres");
	sprintf(worker.bgw_function_name, "AutonomousSessionMain");
	sprintf(worker.bgw_type, "autonomous_transaction");
	snprintf(worker.bgw_name, BGW_MAXLEN, "autonomous session by PID %d", MyProcPid);
	worker.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(seg));
	worker.bgw_notify_pid = MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&worker, &session->worker_handle))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not register background process"),
				 errhint("You might need to increase max_worker_processes.")));

	shm_mq_set_handle(session->command_qh, session->worker_handle);
	shm_mq_set_handle(session->response_qh, session->worker_handle);

	bgwstatus = WaitForBackgroundWorkerStartup(session->worker_handle, &pid);
	if (bgwstatus != BGWH_STARTED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not start background worker")));

	do
	{
		shm_mq_receive_stringinfo(session->response_qh, &msg);
		msgtype = pq_getmsgbyte(&msg);

		switch (msgtype)
		{
			case 'Z': /* ReadyForQuery */
				session->transaction_status = pq_getmsgbyte(&msg);
				pq_getmsgend(&msg);
				break;
			default:
				invalid_protocol_message(msgtype, 1);
				break;
			case 'N': /* NoticeResponse */
				rethrow_errornotice(&msg, NOTICE);
				break;
			case 'E': /* ErrorResponse */
				rethrow_errornotice(&msg, ERROR);
				break;
		}
	}
	while (msgtype != 'Z');

	return session;
}

/*
 * Destroy autonomous session normally
 */
static void
AutonomousSessionEnd(AutonomousSession *session)
{
	StringInfoData msg;
	if (session->transaction_status == 'T')
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("autonomous session ended with transaction block open")));

	pq_redirect_to_shm_mq(session->seg, session->command_qh);
	pq_beginmessage(&msg, 'X'); /* Terminate */
	pq_endmessage(&msg);
	pq_stop_redirect_to_shm_mq();

	AutonomousSessionFreeResources(session);
}

/*
 * Destroy autonomous session in case of ERROR
 */
static void
AutonomousSessionEndError(AutonomousSession *session)
{
	StringInfoData msg;
	pq_redirect_to_shm_mq(session->seg, session->command_qh);
	pq_beginmessage(&msg, 'X'); /* Terminate */
	pq_endmessage(&msg);
	pq_stop_redirect_to_shm_mq();

	AutonomousSessionFreeResources(session);
}

/*
 * Free memory of internal structures of autonomous session
 */
static void
AutonomousSessionFreeResources(AutonomousSession *session)
{
	pfree(session->worker_handle);
	dsm_detach(session->seg);
	pfree(session);
}

AutonomousResult *
AutonomousSessionExecute(AutonomousSession *session, const char *sql)
{
	StringInfoData msg;
	char		msgtype;
	AutonomousResult *result;

	pq_redirect_to_shm_mq(session->seg, session->command_qh);
	pq_beginmessage(&msg, 'Q'); /* Query */
	pq_sendstring(&msg, sql);
	pq_endmessage(&msg);
	pq_stop_redirect_to_shm_mq();

	result = palloc0(sizeof(*result));

	do
	{
		shm_mq_receive_stringinfo(session->response_qh, &msg);
		msgtype = pq_getmsgbyte(&msg);

		switch (msgtype)
		{
			case 'C': /* CommandComplete */
				{
					const char *tag = pq_getmsgstring(&msg);
					result->command = pstrdup(tag);
					pq_getmsgend(&msg);
					break;
				}
			case 'T': /* RowDescription */
				if (result->tupdesc)
					elog(ERROR, "already received a T message");
				result->tupdesc = TupleDesc_from_RowDescription(&msg);
				pq_getmsgend(&msg);
				break;
			case 'D': /* Describe */
				if (!result->tupdesc)
					elog(ERROR, "no T message before D");
				result->tuples = lappend(result->tuples, HeapTuple_from_DataRow(result->tupdesc, &msg));
				pq_getmsgend(&msg);
				break;
			case 'Z': /* ReadyForQuery */
				session->transaction_status = pq_getmsgbyte(&msg);
				pq_getmsgend(&msg);
				break;
			case 'A': /* NotificationResponse */
				forward_NotifyResponse(&msg);
				break;
			case 'N': /* NoticeResponse */
				rethrow_errornotice(&msg, NOTICE);
				break;
			case 'E': /* ErrorResponse */
				rethrow_errornotice(&msg, ERROR);
				break;
			default:
				invalid_protocol_message(msgtype, 2);
				break;
		}
	}
	while (msgtype != 'Z');

	return result;
}

/*
 * Prepare an SQL string for subsequent execution
 */
AutonomousPreparedStatement *
AutonomousSessionPrepare(AutonomousSession *session, const char *sql, int16 nargs,
						 Oid argtypes[], const char *argnames[])
{
	AutonomousPreparedStatement *result;
	StringInfoData msg;
	int16		i;
	char		msgtype;

	pq_redirect_to_shm_mq(session->seg, session->command_qh);
	pq_beginmessage(&msg, 'P'); /* Parse */
	pq_sendstring(&msg, "");
	pq_sendstring(&msg, sql);
	pq_sendint(&msg, nargs, 2);
	for (i = 0; i < nargs; i++)
		pq_sendint(&msg, argtypes[i], 4);
	if (argnames)
		for (i = 0; i < nargs; i++)
			pq_sendstring(&msg, argnames[i]);
	pq_endmessage(&msg);
	pq_stop_redirect_to_shm_mq();

	result = palloc0(sizeof(*result));
	result->session = session;
	result->argtypes = palloc(nargs * sizeof(*result->argtypes));
	memcpy(result->argtypes, argtypes, nargs * sizeof(*result->argtypes));

	do
	{
		shm_mq_receive_stringinfo(session->response_qh, &msg);
		msgtype = pq_getmsgbyte(&msg);

		switch (msgtype)
		{
			case '1': /* ParseComplete */
				break;
			case 'N': /* NoticeResponse */
				rethrow_errornotice(&msg, NOTICE);
				break;
			case 'E': /* ErrorResponse */
				rethrow_errornotice(&msg, ERROR);
				break;
			default:
				invalid_protocol_message(msgtype, 3);
				break;
		}
	}
	while (msgtype != '1');

	pq_redirect_to_shm_mq(session->seg, session->command_qh);
	pq_beginmessage(&msg, 'D'); /* Describe */
	pq_sendbyte(&msg, 'S'); /* PreparedStatement */
	pq_sendstring(&msg, "");
	pq_endmessage(&msg);
	pq_stop_redirect_to_shm_mq();

	do
	{
		shm_mq_receive_stringinfo(session->response_qh, &msg);
		msgtype = pq_getmsgbyte(&msg);

		switch (msgtype)
		{
			case 'n': /* NoData */
				break;
			case 't': /* ParameterDescription */
			case '1': /* ParseComplete */
			case 'Z': /* ReadyForQuery */
				/* ignore for now */
				break;
			case 'T': /* RowDescription */
				if (result->tupdesc)
					elog(ERROR, "already received a T message");
				result->tupdesc = TupleDesc_from_RowDescription(&msg);
				pq_getmsgend(&msg);
				break;
			case 'A': /* NotificationResponse */
				forward_NotifyResponse(&msg);
				break;
			case 'N': /* NoticeResponse */
				rethrow_errornotice(&msg, NOTICE);
				break;
			case 'E': /* ErrorResponse */
				rethrow_errornotice(&msg, ERROR);
				break;
			default:
				invalid_protocol_message(msgtype, 4);
				break;
		}
	}
	while (msgtype != 'n' && msgtype != 'T');

	return result;
}

/*
 * Execute prepared statement
 */
AutonomousResult *
AutonomousSessionExecutePrepared(AutonomousPreparedStatement *stmt, int16 nargs, Datum *values, bool *nulls)
{
	AutonomousSession *session;
	StringInfoData msg;
	AutonomousResult *result;
	char		msgtype;
	int16		i;

	session = stmt->session;

	pq_redirect_to_shm_mq(session->seg, session->command_qh);
	pq_beginmessage(&msg, 'B'); /* Bind */
	pq_sendstring(&msg, "");
	pq_sendstring(&msg, "");
	pq_sendint(&msg, 1, 2);  /* number of parameter format codes */
	pq_sendint(&msg, 1, 2);
	pq_sendint(&msg, nargs, 2);  /* number of parameter values */
	for (i = 0; i < nargs; i++)
	{
		if (nulls[i])
			pq_sendint(&msg, -1, 4);
		else
		{
			Oid			typsend;
			bool		typisvarlena;
			bytea	   *outputbytes;

			getTypeBinaryOutputInfo(stmt->argtypes[i], &typsend, &typisvarlena);
			outputbytes = OidSendFunctionCall(typsend, values[i]);
			pq_sendint(&msg, VARSIZE(outputbytes) - VARHDRSZ, 4);
			pq_sendbytes(&msg, VARDATA(outputbytes), VARSIZE(outputbytes) - VARHDRSZ);
			pfree(outputbytes);
		}
	}
	pq_sendint(&msg, 1, 2);  /* number of result column format codes */
	pq_sendint(&msg, 1, 2);
	pq_endmessage(&msg);
	pq_stop_redirect_to_shm_mq();

	do
	{
		shm_mq_receive_stringinfo(session->response_qh, &msg);
		msgtype = pq_getmsgbyte(&msg);

		switch (msgtype)
		{
			case '2': /* BindComplete */
				break;
			case 'N': /* NoticeResponse */
				rethrow_errornotice(&msg, NOTICE);
				break;
			case 'E': /* ErrorResponse */
				rethrow_errornotice(&msg, ERROR);
				break;
			default:
				invalid_protocol_message(msgtype, 5);
				break;
		}
	}
	while (msgtype != '2');

	pq_redirect_to_shm_mq(session->seg, session->command_qh);
	pq_beginmessage(&msg, 'E'); /* Execute */
	pq_sendstring(&msg, "");
	pq_sendint(&msg, 0, 4);
	pq_endmessage(&msg);
	pq_stop_redirect_to_shm_mq();

	result = palloc0(sizeof(*result));
	result->tupdesc = stmt->tupdesc;

	do
	{
		shm_mq_receive_stringinfo(session->response_qh, &msg);
		msgtype = pq_getmsgbyte(&msg);

		switch (msgtype)
		{
			case '2': /* BindComplete */
				break;
			case 'C': /* CommandComplete */
				{
					const char *tag = pq_getmsgstring(&msg);
					result->command = pstrdup(tag);
					pq_getmsgend(&msg);
					break;
				}
			case 'D': /* Describe */
				if (!stmt->tupdesc)
					elog(ERROR, "did not expect any description rows");
				result->tuples = lappend(result->tuples, HeapTuple_from_DataRow(stmt->tupdesc, &msg));
				pq_getmsgend(&msg);
				break;
			case 'A': /* NotificationResponse */
				forward_NotifyResponse(&msg);
				break;
			case 'N': /* NoticeResponse */
				rethrow_errornotice(&msg, NOTICE);
				break;
			case 'E': /* ErrorResponse */
				rethrow_errornotice(&msg, ERROR);
				break;
			default:
				invalid_protocol_message(msgtype, 6);
				break;
		}
	}
	while (msgtype != 'C');

	pq_redirect_to_shm_mq(session->seg, session->command_qh);
	pq_putemptymessage('S'); /* Sync */
	pq_stop_redirect_to_shm_mq();

	do
	{
		shm_mq_receive_stringinfo(session->response_qh, &msg);
		msgtype = pq_getmsgbyte(&msg);

		switch (msgtype)
		{
			case 'Z': /* ReadyForQuery */
				session->transaction_status = pq_getmsgbyte(&msg);
				pq_getmsgend(&msg);
				break;
			case 'A': /* NotificationResponse */
				forward_NotifyResponse(&msg);
				break;
			case 'N': /* NoticeResponse */
				rethrow_errornotice(&msg, NOTICE);
				break;
			case 'E': /* ErrorResponse */
				rethrow_errornotice(&msg, ERROR);
				break;
			default:
				invalid_protocol_message(msgtype, 7);
				break;
		}
	}
	while (msgtype != 'Z');

	return result;
}

/*
 * Main loop of autonomous session
 */
void
AutonomousSessionMain(Datum main_arg)
{
	dsm_segment *seg;
	shm_toc	   *toc;
	AutonomousSessionFixedData *fdata;
	char	   *gucstate;
	shm_mq	   *command_mq;
	shm_mq	   *response_mq;
	shm_mq_handle *command_qh;
	shm_mq_handle *response_qh;
	StringInfoData msg;
	char		msgtype;

	pqsignal(SIGTERM, die);

	BackgroundWorkerUnblockSignals();

	/* Set up a memory context and resource owner. */
	Assert(CurrentResourceOwner == NULL);
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "autonomous");
	CurrentMemoryContext = AllocSetContextCreate(TopMemoryContext,
												 "autonomous session",
												 ALLOCSET_DEFAULT_MINSIZE,
												 ALLOCSET_DEFAULT_INITSIZE,
												 ALLOCSET_DEFAULT_MAXSIZE);

	init_row_description_buf();

	seg = dsm_attach(DatumGetInt32(main_arg));
	if (seg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not map dynamic shared memory segment")));

	toc = shm_toc_attach(AUTONOMOUS_MAGIC, dsm_segment_address(seg));
	if (toc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("bad magic number in dynamic shared memory segment")));

	/* Find data structures in dynamic shared memory. */
	fdata = shm_toc_lookup(toc, AUTONOMOUS_KEY_FIXED_DATA, false);

	gucstate = shm_toc_lookup(toc, AUTONOMOUS_KEY_GUC, false);

	command_mq = shm_toc_lookup(toc, AUTONOMOUS_KEY_COMMAND_QUEUE, false);
	shm_mq_set_receiver(command_mq, MyProc);
	command_qh = shm_mq_attach(command_mq, seg, NULL);

	response_mq = shm_toc_lookup(toc, AUTONOMOUS_KEY_RESPONSE_QUEUE, false);
	shm_mq_set_sender(response_mq, MyProc);
	response_qh = shm_mq_attach(response_mq, seg, NULL);

	pq_redirect_to_shm_mq(seg, response_qh);
	BackgroundWorkerInitializeConnectionByOid(fdata->database_id,
											  fdata->authenticated_user_id, 0);
	SetClientEncoding(GetDatabaseEncoding());

	StartTransactionCommand();
	RestoreGUCState(gucstate);
	CommitTransactionCommand();

	process_session_preload_libraries();

	SetUserIdAndSecContext(fdata->current_user_id, fdata->sec_context);

	whereToSendOutput = DestRemote;
	ReadyForQuery(whereToSendOutput); /* Z ReadyForQuery*/

	MessageContext = AllocSetContextCreate(TopMemoryContext,
										   "MessageContext",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);

	do
	{
		MemoryContextSwitchTo(MessageContext);
		MemoryContextReset(MessageContext);
		InvalidateCatalogSnapshotConditionally();

		pgstat_report_stat(false);
		pgstat_report_activity(STATE_IDLE, NULL);

		shm_mq_receive_stringinfo(command_qh, &msg);
		msgtype = pq_getmsgbyte(&msg);

		switch (msgtype)
		{
			case 'B':
				{
					SetCurrentStatementStartTimestamp();
					exec_bind_message(&msg);
					break;
				}
			case 'D': /* Describe */
				{
					int	 describe_type;
					const char *describe_target;

					SetCurrentStatementStartTimestamp();

					describe_type = pq_getmsgbyte(&msg);
					describe_target = pq_getmsgstring(&msg);
					pq_getmsgend(&msg);

					switch (describe_type)
					{
						case 'S': /* PreparedStatement */
							exec_describe_statement_message(describe_target);
							break;
#ifdef XXX
						case 'P': /* Portal */
							exec_describe_portal_message(describe_target);
							break;
#endif
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
									 errmsg("invalid DESCRIBE message subtype %d",
											describe_type)));
							break;
					}
				}
				break;
			case 'E': /* Execute */
				{
					const char *portal_name;
					int			max_rows;

					SetCurrentStatementStartTimestamp();

					portal_name = pq_getmsgstring(&msg);
					max_rows = pq_getmsgint(&msg, 4);
					pq_getmsgend(&msg);

					exec_execute_message(portal_name, max_rows);
				}
				break;

			case 'P': /* Parse */
				{
					const char *stmt_name;
					const char *query_string;
					int			numParams;
					Oid		   *paramTypes = NULL;
					const char **paramNames = NULL;

					SetCurrentStatementStartTimestamp();

					stmt_name = pq_getmsgstring(&msg);
					query_string = pq_getmsgstring(&msg);
					numParams = pq_getmsgint(&msg, 2);
					if (numParams > 0)
					{
						int			i;

						paramTypes = palloc(numParams * sizeof(Oid));
						for (i = 0; i < numParams; i++)
							paramTypes[i] = pq_getmsgint(&msg, 4);
					}
					/* If data left in message, read parameter names. */
					if (msg.cursor != msg.len)
					{
						int			i;

						paramNames = palloc(numParams * sizeof(char *));
						for (i = 0; i < numParams; i++)
							paramNames[i] = pq_getmsgstring(&msg);
					}
					pq_getmsgend(&msg);

					exec_parse_message(query_string, stmt_name,
									   paramTypes, numParams, paramNames);
					break;
				}
			case 'Q': /* Query */
				{
					const char *sql;
					int save_log_statement;
					bool save_log_duration;
					int save_log_min_duration_statement;

					sql = pq_getmsgstring(&msg);
					pq_getmsgend(&msg);

					/* XXX room for improvement */
					save_log_statement = log_statement;
					save_log_duration = log_duration;
					save_log_min_duration_statement = log_min_duration_statement;

					check_client_encoding_hook = autonomous_check_client_encoding_hook;
					log_statement = LOGSTMT_NONE;
					log_duration = false;
					log_min_duration_statement = -1;

					SetCurrentStatementStartTimestamp();
					exec_simple_query(sql, 1);

					log_statement = save_log_statement;
					log_duration = save_log_duration;
					log_min_duration_statement = save_log_min_duration_statement;
					check_client_encoding_hook = NULL;

					ReadyForQuery(whereToSendOutput);
					break;
				}
			case 'S': /* Sync */
				{
					pq_getmsgend(&msg);
					finish_xact_command();
					ReadyForQuery(whereToSendOutput);
					break;
				}
			case 'X': /* Terminate */
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid protocol message type from autonomous session leader: %c",
								msgtype)));
				break;
		}
	}
	while (msgtype != 'X');
}


static void
shm_mq_receive_stringinfo(shm_mq_handle *qh, StringInfoData *msg)
{
	Size		nbytes;
	void		*data;

	shm_mq_result res = shm_mq_receive(qh, &nbytes, &data, false);
	if (res != SHM_MQ_SUCCESS)
		elog(ERROR, "shm_mq_receive failed: %d", res);

	initStringInfo(msg);
	appendBinaryStringInfo(msg, data, nbytes);
}


static void
autonomous_check_client_encoding_hook(void)
{
	elog(ERROR, "cannot set client encoding in autonomous session");
}


static TupleDesc
TupleDesc_from_RowDescription(StringInfo msg)
{
	int16		natts = pq_getmsgint(msg, 2);
	TupleDesc	tupdesc = CreateTemplateTupleDesc(natts);
	
	for (int16 i = 0; i < natts; i++)
	{
		const char *colname;
		Oid     type_oid;
		int32	typmod;
		int16	format;

		colname = pq_getmsgstring(msg);
		(void) pq_getmsgint(msg, 4);   /* table OID */
		(void) pq_getmsgint(msg, 2);   /* table attnum */
		type_oid = pq_getmsgint(msg, 4);
		(void) pq_getmsgint(msg, 2);   /* type length */
		typmod = pq_getmsgint(msg, 4);
		format = pq_getmsgint(msg, 2);
		(void) format;
		/* XXX The protocol sometimes sends 0 (text) if the format is not
		 * determined yet. Fix why it can't be determined.
		 * We always use binary, 1.
		 */

		TupleDescInitEntry(tupdesc, i + 1, colname, type_oid, typmod, 0);
	}
	return tupdesc;
}


static HeapTuple
HeapTuple_from_DataRow(TupleDesc tupdesc, StringInfo msg)
{
	int16		natts = pq_getmsgint(msg, 2);
	Datum		*values;
	bool		*nulls;
	StringInfoData buf;

	Assert(tupdesc);

	if (natts != tupdesc->natts)
		elog(ERROR, "malformed DataRow");

	values = palloc(natts * sizeof(*values));
	nulls = palloc(natts * sizeof(*nulls));
	initStringInfo(&buf);

	for (int16 i = 0; i < natts; i++)
	{
		int32 len = pq_getmsgint(msg, 4);

		if (len < 0)
			nulls[i] = true;
		else
		{
			Oid recvid;
			Oid typioparams;

			nulls[i] = false;

			getTypeBinaryInputInfo(tupdesc->attrs[i].atttypid,
								   &recvid,
								   &typioparams);
			resetStringInfo(&buf);
			appendBinaryStringInfo(&buf, pq_getmsgbytes(msg, len), len);
			values[i] = OidReceiveFunctionCall(recvid, &buf, typioparams,
											   tupdesc->attrs[i].atttypmod);
		}
	}

	return heap_form_tuple(tupdesc, values, nulls);
}


static void
forward_NotifyResponse(StringInfo msg)
{
	int32	pid;
	const char *channel;
	const char *payload;

	pid = pq_getmsgint(msg, 4);
	channel = pq_getmsgrawstring(msg);
	payload = pq_getmsgrawstring(msg);
	pq_endmessage(msg);

	NotifyMyFrontEnd(channel, payload, pid);
}

static void
rethrow_errornotice(StringInfo msg, int min_elevel)
{
	ErrorData	edata;

	pq_parse_errornotice(msg, &edata);
	edata.elevel = Min(edata.elevel, min_elevel);
	if(edata.elevel >= ERROR) {
		slist_node *node = slist_pop_head_node(&AutonomousStack);
		AutonomousSession *session = slist_container(AutonomousStackNode, node, node)->session;
		AutonomousSessionRelease(session);
		pfree(node);
	}
	ThrowErrorData(&edata);
}


static void
invalid_protocol_message(char msgtype, int phase)
{
	ereport(ERROR,
			(errcode(ERRCODE_PROTOCOL_VIOLATION),
			 errmsg("invalid protocol message type %c from autonomous session during phase %d",
					msgtype, phase)));
}
