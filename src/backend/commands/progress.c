/*
 * progress.c
 *	  Monitor progression of request: PROGRESS
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/monitor.c
 */

#include "postgres.h"

#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#include "nodes/nodes.h"
#include "tcop/dest.h"
#include "tcop/pquery.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "executor/progress.h"
#include "access/xact.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "storage/lmgr.h"
#include "storage/latch.h"
#include "storage/procsignal.h"
#include "storage/backendid.h"
#include "executor/execdesc.h"
#include "executor/executor.h"
#include "executor/hashjoin.h"
#include "executor/execParallel.h"
#include "commands/defrem.h"
#include "access/relscan.h"
#include "access/parallel.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/tuplesort.h"
#include "utils/tuplestore.h"
#include "storage/buffile.h"
#include "utils/ruleutils.h"
#include "postmaster/bgworker_internals.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "funcapi.h"
#include "pgstat.h"

static int log_stmt = 1;		/* log query monitored */
static int debug = 1;

/* 
 * Monitoring progress waits 5secs for monitored backend response.
 *
 * If this timeout is too short, it may not leave enough time for monotored backend to dump its
 * progression about the SQL query it is running.
 *
 * If this timeout is too long, a cancelled SQL query in a backend could block the monitoring
 * backend too for a longi time.
 */
unsigned short PROGRESS_TIMEOUT = 10;
unsigned short PROGRESS_TIMEOUT_CHILD = 5;
char* progress_backend_timeout = "<backend timeout>";

/*
 * Backend type (single worker, parallel main worker, parallel child worker
 */
#define SINGLE_WORKER	0
#define MAIN_WORKER	1
#define CHILD_WORKER	2

/*
 * Number of colums for pg_progress SQL function
 */
#define PG_PROGRESS_COLS	9

#define PG_PROGRESS_PID		9
#define PG_PROGRESS_BID		9
#define PG_PROGRESS_LINEID 	9
#define PG_PROGRESS_INDENT	9
#define PG_PROGRESS_TYPE	65
#define PG_PROGRESS_NAME	257
#define PG_PROGRESS_VALUE	257
#define PG_PROGRESS_UNIT	9

/*
 * Progress node type
 */
#define PROP			"property"
#define NODE			"node"
#define RELATIONSHIP		"relationship"

/*
 * Units for reports
 */
#define NO_UNIT			""
#define BLK_UNIT		"block"
#define ROW_UNIT		"row"
#define PERCENT_UNIT		"percent"
#define SECOND_UNIT		"second"
#define BYTE_UNIT		"byte"
#define KBYTE_UNIT		"KByte"
#define MBYTE_UNIT		"MByte"
#define GBYTE_UNIT		"Gbyte"
#define TBYTE_UNIT		"TByte"

/*
 * Verbosity report level
 */
#define VERBOSE_DISK_USE		1
#define VERBOSE_ROW_SCAN		2
#define VERBOSE_INDEX_SCAN		2
#define	VERBOSE_BUFFILE			2
#define VERBOSE_HASH_JOIN		2
#define VERBOSE_HASH_JOIN_DETAILED	3
#define VERBOSE_TAPES			2
#define VERBOSE_TAPES_DETAILED		3
#define VERBOSE_TIME_REPORT		1
#define VERBOSE_STACK			3

/*
 * Report only SQL querries which have been running longer than this value
 */
int progress_time_threshold = 3;

/*
 * One ProgressCtl is allocated for each backend process which is to be potentially monitored
 * The array of progress_ctl structures is protected by ProgressLock global lock.
 *
 * Only one backend can be monitored at a time. This may be improved with a finer granulary
 * using a LWLock tranche of MAX_NR_BACKENDS locks. In which case, one backend can be monitored
 * independantly of the otther backends.
 *
 * The LWLock ensure that one backend can be only monitored by one other backend at a time.
 * Other backends trying to monitor an already monitered backend will be put in
 * queue of the LWWlock.
 */
typedef struct ProgressCtl {
        bool verbose;			/* be verbose */

	bool parallel;			/* true if parallel query */
	bool child;			/* true if child worker, false if main worker */
	unsigned int child_indent;	/* Indentation based value for child worker */

	unsigned long disk_size;	/* Disk size in bytes used by the backend for sorts, stores, hashes */

	char* buf;			/* progress status report in shm */
	int pid[MAX_PARALLEL_WORKER_LIMIT];
	
	struct Latch* latch;		/* Used by requestor to wait for backend to complete its report */
} ProgressCtl;

struct ProgressCtl* progress_ctl_array;	/* Array of MaxBackends ProgressCtl */
char* dump_buf_array;			/* SHMEM buffers one for each backend */
struct Latch* resp_latch_array;		/* Array of MaxBackends latches to synchronize response
					 * from monitored backend to monitoring backend */

typedef struct ProgressState {
	bool parallel;			/* true if parallel query */
	bool child;			/* true if parallel and child backend */

	int pid;			/* pid of backend of child worker if parallel */
	int ppid;			/* pid of parent worker */
	int bid;

	/*
	 * State for output formating
	 */
	int indent;			/* current indentation level */
	int lineid;			/* needed for indentation */
	bool verbose;			/* be verbose */
	StringInfo str;			/* output buffer */

	List* grouping_stack;		/* format-specific grouping state */
 
	MemoryContext memcontext;

	/*
	 * State related to current plan/execution tree
	 */
	PlannedStmt* pstmt;
	struct Plan* plan;
	struct PlanState* planstate;
	List* rtable;
	List* rtable_names;
	List* deparse_cxt;		/* context list for deparsing expressions */
	EState* es;			/* Top level data */
	Bitmapset* printed_subplans;    /* ids of SubPlans we've printed */

	unsigned long disk_size;        /* track on disk use for sorts, stores, and hashes */
} ProgressState;

/*
 * No progress request unless requested.
 */
volatile bool progress_requested = false;

/*
 * local functions
 */
static void ProgressPlan(QueryDesc* query, ProgressState* ps);
static void ProgressNode(PlanState* planstate, List* ancestors,
	const char* relationship, const char* plan_name, ProgressState* ps);

static ProgressState* CreateProgressState(void);
static void ProgressIndent(ProgressState* ps);
static void ProgressUnindent(ProgressState* ps);

static void ProgressPid(int pid, int ppid, int verbose, Tuplestorestate* tupstore,
	TupleDesc tupdesc, char* buf);
static void ProgressSpecialPid(int pid, int bid, Tuplestorestate* tupstore, TupleDesc tupdesc, char* buf);

static bool ReportHasChildren(Plan* plan, PlanState* planstate);

/*
 * Individual nodes of interest are:
 * - scan data: for heap or index
 * - sort data: for any relation or tuplestore
 * Other nodes only wait on above nodes
 */
static void ProgressGather(GatherState* gs, ProgressState* ps);
static void ProgressGatherMerge(GatherMergeState* gs, ProgressState* ps);
static void ProgressParallelExecInfo(ParallelContext* pc, ProgressState* ps);

static void ProgressScanBlks(ScanState* ss, ProgressState* ps);
static void ProgressScanRows(Scan* plan, PlanState* plantstate, ProgressState* ps);
static void ProgressTidScan(TidScanState* ts, ProgressState* ps);
static void ProgressCustomScan(CustomScanState* cs, ProgressState* ps);
static void ProgressIndexScan(IndexScanState* is, ProgressState* ps); 

static void ProgressLimit(LimitState* ls, ProgressState* ps);
static void ProgressModifyTable(ModifyTableState * planstate, ProgressState* ps);
static void ProgressHashJoin(HashJoinState* planstate, ProgressState* ps);
static void ProgressHash(HashState* planstate, ProgressState* ps);
static void ProgressHashJoinTable(HashJoinTable hashtable, ProgressState* ps);
static void ProgressBufFileRW(BufFile* bf, ProgressState* ps, unsigned long *reads,
	unsigned long * writes, unsigned long *disk_size);
static void ProgressBufFile(BufFile* bf, ProgressState* ps);
static void ProgressMaterial(MaterialState* planstate, ProgressState* ps);
static void ProgressTupleStore(Tuplestorestate* tss, ProgressState* ps);
static void ProgressAgg(AggState* planstate, ProgressState* ps);
static void ProgressSort(SortState* ss, ProgressState* ps);
static void ProgressTupleSort(Tuplesortstate* tss, ProgressState* ps); 
static void dumpTapes(struct ts_report* tsr, ProgressState* ps);

//extern void ReportText(const char* label, const char* value, ReportState* rpt);
//extern void ReportTextNoNewLine(const char* label, const char* value, ReportState* rpt);

static void ReportTime(QueryDesc* query, ProgressState* ps);
static void ReportStack(ProgressState* ps);
static void ReportDisk(ProgressState* ps);

static void ProgressDumpRequest(int pid);
static void ProgressResetRequest(ProgressCtl* req);

static void ProgressPropLong(ProgressState* ps, const char* type,
	const char* name, unsigned long value, const char* unit);
static void ProgressPropText(ProgressState* ps, const char* type,
	const char* name, const char* value);
static void ProgressPropTextStr(StringInfo str, int pid, int bid,
	int lineid, int indent, const char* type, const char* name, const char* value);



Size ProgressShmemSize(void)
{
	Size size;

	/* Must match ProgressShmemInit */
	size = mul_size(MaxBackends, sizeof(ProgressCtl));
	size = add_size(size, mul_size(MaxBackends, PROGRESS_AREA_SIZE));
	size = add_size(size, mul_size(MaxBackends, sizeof(struct Latch)));

	return size;
}

/*
 * Initialize our shared memory area
 */
void ProgressShmemInit(void)
{
	bool found;
	size_t size = 0;

	/*
 	 * Allocated shared latches for response to progress request
 	 */
	size = mul_size(MaxBackends, sizeof(struct Latch));
	resp_latch_array = ShmemInitStruct("Progress latches", size, &found);
	if (!found) {
		int i;
		struct Latch* l;

		l = resp_latch_array;
		for (i = 0; i < MaxBackends; i++) {
			InitSharedLatch(l);
			l++;
		}	
	}

	/*
	 * Allocate SHMEM buffers for backend communication
	 */
	size = MaxBackends * PROGRESS_AREA_SIZE;
	dump_buf_array = (char*) ShmemInitStruct("Backend Dump Pages", size, &found);
	if (!found) {
        	memset(dump_buf_array, 0, size);
	}

	/*
	 * Allocate progress request meta data, one for each backend
	 */
	size = mul_size(MaxBackends, sizeof(ProgressCtl));
	progress_ctl_array = ShmemInitStruct("ProgressCtl array", size, &found);
	if (!found) {
		int i;
		ProgressCtl* req;
		struct Latch* latch;

		req = progress_ctl_array;
		latch = resp_latch_array;
		for (i = 0; i < MaxBackends; i++) {
			/* Already zeroed above */
			memset(req, 0, sizeof(ProgressCtl));
	
			/* set default value */
			req->latch = latch;
			req->buf = dump_buf_array + i * PROGRESS_AREA_SIZE;
			req->parallel = false;
			req->child = false;
			req->child_indent = 0;
			req->disk_size = 0;
			req->verbose = 0;
			memset(req->pid, 0, sizeof(int) * MAX_PARALLEL_WORKER_LIMIT);
			req++;
			latch++;
		}
	}

	return;
}

/*
 * Dump request management
 */
static
void ProgressDumpRequest(int pid)
{
	int bid;
	ProgressCtl* req;

	bid = ProcPidGetBackendId(pid);
	req = progress_ctl_array + bid;
	elog(LOG, "backend pid=%d bid=%d verbose=%d, parallel=%d child= %d indent=%d",
		pid, bid, req->verbose, req->parallel, req->child, req->child_indent);
}

static
void ProgressResetRequest(ProgressCtl* req)
{
	elog(LOG, "reset progress request at addr %p", req);

	req->parallel = false;
	req->child = false;
	req->child_indent = 0;
	req->disk_size = 0;
	req->verbose = 0;

	InitSharedLatch(req->latch);

	memset(req->buf, 0, PROGRESS_AREA_SIZE);
	memset(req->pid, 0, sizeof(int) * MAX_PARALLEL_WORKER_LIMIT);
}

/*
 * Report of rows in pg_progress tables
 */
static
void ProgressPropLong(ProgressState* ps,
	const char* type, const char* name, unsigned long value, const char* unit)
{
	/*
	 * Fields are: pid, lineid, indent, name, value, unit
	 */
	char pid_str[PG_PROGRESS_PID];
	char bid_str[PG_PROGRESS_BID];
	char lineid_str[PG_PROGRESS_LINEID];
	char indent_str[PG_PROGRESS_INDENT];
	char value_str[PG_PROGRESS_VALUE];

	sprintf(pid_str, "%d", ps->pid);
	sprintf(bid_str, "%d", ps->bid);
	sprintf(lineid_str, "%d", ps->lineid);
	sprintf(indent_str, "%d", ps->indent);
	sprintf(value_str, "%lu", value);

	elog(LOG, "ProgressPropLong PID_STR = %s", pid_str);	
	appendStringInfo(ps->str, "%s|%s|%s|%s|%s|%s|%s|%s|",
		pid_str, bid_str, lineid_str, indent_str, type, name, value_str, unit);

	ps->lineid++;
}

static
void ProgressPropText(ProgressState* ps,
	const char* type, const char* name, const char* value)
{
	/*
	 * Fields are: pid, lineid, indent, name, value, unit
	 */
	char pid_str[PG_PROGRESS_PID];
	char bid_str[PG_PROGRESS_BID];
	char lineid_str[PG_PROGRESS_LINEID];
	char indent_str[PG_PROGRESS_INDENT];

	sprintf(pid_str, "%d", ps->pid);
	sprintf(bid_str, "%d", ps->bid);
	sprintf(lineid_str, "%d", ps->lineid);
	sprintf(indent_str, "%d", ps->indent);
	
	elog(LOG, "ProgressPropText PID_STR = %s", pid_str);	
	appendStringInfo(ps->str, "%s|%s|%s|%s|%s|%s|%s||",
		pid_str, bid_str, lineid_str, indent_str, type, name, value);

	ps->lineid++;
}

static
void ProgressPropTextStr(StringInfo str, int pid, int bid, int lineid,
	int indent, const char* type, const char* name, const char* value)
{
	elog(LOG, "ProgressPropTextStr PID_STR = %d", pid);	
	appendStringInfo(str, "%d|%d|%d|%d|%s|%s|%s||",
		pid, bid, lineid, indent, type, name, value);
}

static
void ProgressResetReport(
	ProgressState* ps)
{
	resetStringInfo(ps->str);
}

/*
 * Colums are: pid, lineid, indent, property, value, unit
 */
Datum pg_progress(PG_FUNCTION_ARGS)
{
	int pid;
	char* buf;

	unsigned short verbose;

	Datum values[PG_PROGRESS_COLS];
	bool nulls[PG_PROGRESS_COLS];
	TupleDesc tupdesc;
	Tuplestorestate* tupstore;
	ReturnSetInfo* rsinfo;

	ProgressCtl* req;
	ProgressCtl* child_req;
	int pid_index;	
	int child_pid;	

	BackendId child_bid;

	int num_backends;
	int curr_backend;

	MemoryContext per_query_ctx;
	MemoryContext oldcontext;


	if (debug)
		elog(LOG, "Start of pg_progress");

	/*
	 * pid = 0 means collect progress report for all backends
	 */
	pid = PG_ARGISNULL(0) ? 0 : PG_GETARG_INT32(0);
	verbose = PG_ARGISNULL(0) ? false : PG_GETARG_UINT16(1);
	if (debug)
		elog(LOG, "pid = %d, verbose = %d", pid, verbose);
	
	/*
	 * Build a tuple descriptor for our result type
	 */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE) {
		elog(ERROR, "return type must be a row type");
	}

	/*
	 * Switch to query memory context
	 */
	rsinfo = (ReturnSetInfo*) fcinfo->resultinfo;
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, 0, sizeof(nulls));

	/* Allocate buf for local work */
	buf = palloc0(PROGRESS_AREA_SIZE);

	if (pid > 0) {
		/* Target specific pid given as SQL function argupment */
		ProgressPid(pid, 0, verbose, tupstore, tupdesc, buf);
	} else {
		/* Loop over all backends */
		num_backends = pgstat_fetch_stat_numbackends();
		elog(LOG, "Num backends = %d", num_backends);
		for (curr_backend = 1; curr_backend <= num_backends; curr_backend++) {
			LocalPgBackendStatus* local_beentry;
			PgBackendStatus* beentry;
			TimestampTz delta;
			BackendType backend_type;
			long secs;
			int usecs;

			elog(LOG, "LOOP on backend %d", curr_backend);		
			local_beentry = pgstat_fetch_stat_local_beentry(curr_backend);
			if (!local_beentry) {
				char lbuf[] = "<backend information not available>";

				ProgressSpecialPid(0, curr_backend, tupstore, tupdesc, lbuf);
				continue;
			}

			beentry = &local_beentry->backendStatus;
			pid = beentry->st_procpid;
			backend_type = beentry->st_backendType;

			/*
			 * Do not monitor oneself
			 */
			if (pid == getpid()) {
				// ProgressSpecialPid(pid, curr_backend, tupstore, tupdesc, "<self backend>");
				continue;
			}
				
			if (backend_type != B_BACKEND) {
				// ProgressSpecialPid(pid, curr_backend, tupstore, tupdesc, "<not a standard backend>");
				continue;
			}

			/*
			 * Do not report SQL queries as long as they have not run for threshold amount of time
			 */
			delta = GetCurrentTimestamp() - beentry->st_proc_start_timestamp;
			TimestampDifference(beentry->st_activity_start_timestamp, GetCurrentTimestamp(), &secs, &usecs);
			elog(LOG, "Current %lu Start %lu delta %lu secs %ld",
				GetCurrentTimestamp(), beentry->st_proc_start_timestamp, delta, secs);

			if (secs < progress_time_threshold) {
				char lbuf[] = "<backend has not run long enough>";

				ProgressSpecialPid(pid, curr_backend, tupstore, tupdesc, lbuf);
				continue;
			}

			elog(LOG, "PROGRESS on backend pid %d bid %d", pid, curr_backend);		

			/* 
			 * We need to make sure the buffer is wiped out.
			 * Otherwise previous content may reappear in the the rows output
			 */
			memset(buf, 0, PROGRESS_AREA_SIZE);
			req = progress_ctl_array + curr_backend;
			ProgressResetRequest(req);
			ProgressPid(pid, 0, verbose, tupstore, tupdesc, buf);

			/*
			 * Check for child pid of current pid
			 */
			pid_index = 0;
			while (req->pid[pid_index] != 0) {
				memset(buf, 0, PROGRESS_AREA_SIZE);
				child_pid = req->pid[pid_index];
				child_bid = ProcPidGetBackendId(child_pid);

				child_req = progress_ctl_array + child_bid;
				child_req->parallel = true;
				child_req->child = true;
				child_req->child_indent = req->child_indent;

				elog(LOG, "COLLECT CHILD DATA");
				ProgressDumpRequest(pid);
				ProgressDumpRequest(child_pid);

				ProgressPid(child_pid, pid, verbose, tupstore, tupdesc, buf);
				ProgressResetRequest(child_req);

				pid_index++;
			}
	
			ProgressResetRequest(req);
			ProgressDumpRequest(pid);
		}
	}

	tuplestore_donestoring(tupstore);

	pfree(buf);
	MemoryContextSwitchTo(oldcontext);

	return (Datum) 0;
}

static
void ProgressPid(int pid, int ppid, int verbose, Tuplestorestate* tupstore, TupleDesc tupdesc, char* buf)
{
	int bid;

	Datum values[PG_PROGRESS_COLS];
	bool nulls[PG_PROGRESS_COLS];

	char pid_str[PG_PROGRESS_PID];
	int pid_val;

	char bid_str[PG_PROGRESS_BID];
	int bid_val;

	char lineid_str[PG_PROGRESS_LINEID];
	int lineid_val;

	char indent_str[PG_PROGRESS_INDENT];
	int indent_val;

	char type_str[PG_PROGRESS_TYPE];
	char name_str[PG_PROGRESS_NAME];
	char value_str[PG_PROGRESS_VALUE];
	char unit_str[PG_PROGRESS_UNIT];

	char* token_start;
	char* token_next;
	unsigned short token_length;
	unsigned short total_length;
	int i;

	/* Convert pid to backend_id */
	bid = ProcPidGetBackendId(pid);
	if (bid == InvalidBackendId) {
		ereport(ERROR, (
       		errcode(ERRCODE_INTERVAL_FIELD_OVERFLOW),
		errmsg("Invalid backend process pid")));
	}

	if (pid == getpid()) {
		ereport(ERROR, (
		errcode(ERRCODE_INTERVAL_FIELD_OVERFLOW),
		errmsg("Cannot request status from self")));
	}

	if (debug)
		elog(LOG, "pid = %d, bid = %d", pid, bid);

	ProgressFetchReport(pid, bid, verbose, buf);

	/*
	 * Setup tuple store
	 */
	if (debug) {
		elog(LOG, "setting up tuplestore");
		elog(LOG, "buffer content: %s", buf);
	}

	//ProgressTupstoreFill(buf);
	token_start = buf;
	token_next = buf;

	token_next = strchr(token_start, '|');
	total_length = strlen(buf);
	
	i = 0;

	while (token_start != NULL) {
		token_length = (unsigned short)(token_next - token_start);

		switch(i) {
                case 0:
                        snprintf(pid_str, token_length + 1, "%s", token_start);
                        pid_str[token_length + 1] = '\0';
                        break;
                case 1:
                        snprintf(bid_str, token_length + 1, "%s", token_start);
                        bid_str[token_length + 1] = '\0';
                        break;
                case 2:
                        snprintf(lineid_str, token_length + 1, "%s", token_start);
                        lineid_str[token_length + 1] = '\0';
                        break;
                case 3:
                        snprintf(indent_str, token_length + 1, "%s", token_start);
                        indent_str[token_length + 1] = '\0';
                        break;
                case 4:
                        snprintf(type_str, token_length + 1, "%s", token_start);
                        type_str[token_length + 1] = '\0';
                        break;
                case 5:
                        snprintf(name_str, token_length + 1, "%s", token_start);
                        name_str[token_length + 1] = '\0';
                        break;
                case 6:
                        snprintf(value_str, token_length + 1, "%s", token_start);
                        value_str[token_length + 1] = '\0';
                        break;
                case 7:
                        snprintf(unit_str, token_length + 1, "%s", token_start);
                        unit_str[token_length + 1] = '\0';
                        break;
                };

                i++;

                if (i == 8) {
			/* PK */
			pid_val = atoi(pid_str);	
			values[0] = Int32GetDatum(pid_val);
			nulls[0] = false;

			values[1] = Int32GetDatum(ppid);
			nulls[1] = false;

			/* PK */
			bid_val = atoi(bid_str);	
			values[2] = Int32GetDatum(bid_val);
			nulls[2] = false;

			/* PK */
			lineid_val = atoi(lineid_str);
			values[3] = Int32GetDatum(lineid_val);
			nulls[3] = false;

			/* PK */
			indent_val = atoi(indent_str);
			values[4] = Int32GetDatum(indent_val);
			nulls[4] = false;

			/* PK */
			values[5] = CStringGetTextDatum(type_str);
			nulls[5] = false;

			/* PK */
			values[6] = CStringGetTextDatum(name_str);
			nulls[6] = false;

			if (strlen(value_str) == 0) {
				nulls[7] = true;
			} else {
				values[7] = CStringGetTextDatum(value_str);
				nulls[7] = false;
			}

			if (strlen(unit_str) == 0) {
				nulls[8] = true;
			} else {
				values[8] = CStringGetTextDatum(unit_str);
				nulls[8] = false;
			}

			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
                        i = 0;
                }

		token_start = token_next + 1;
		if (token_start >= buf + total_length)
			break;

                token_next = strchr(token_next + 1,'|');
       }
}

static
void ProgressSpecialPid(int pid, int bid, Tuplestorestate* tupstore, TupleDesc tupdesc, char* buf)
{
	Datum values[PG_PROGRESS_COLS];
	bool nulls[PG_PROGRESS_COLS];

	values[0] = Int32GetDatum(pid);
	nulls[0] = false;
				
	values[1] = Int32GetDatum(0);
	nulls[1] = false;
				
	values[2] = Int32GetDatum(bid);
	nulls[2] = false;
				
	values[3] = Int32GetDatum(0);
	nulls[3] = false;

	values[4] = Int32GetDatum(0);
	nulls[4] = false;

	values[5] = CStringGetTextDatum(PROP);
	nulls[5] = false;

	values[6] = CStringGetTextDatum(buf);
	nulls[6] = false;

	nulls[7] = true;
	nulls[8] = true;

	tuplestore_putvalues(tupstore, tupdesc, values, nulls);
}

/*
 * ProgressFetchReport:
 * 	Log a request to a backend in order to fetch its progress log
 */
void ProgressFetchReport(int pid, int bid, int verbose, char* buf)
{
	ProgressCtl* req;
	unsigned int buf_len = 0;
	unsigned int str_len = 0;
	StringInfo str;
	unsigned short progress_did_timeout = 0;

	if (debug)
		elog(LOG, "Start of ProgressFetchReport with pid = %d, bid = %d", pid, bid);

	/*
	 * Serialize signals/request to get the progress state of the query
	 */
	LWLockAcquire(ProgressLock, LW_EXCLUSIVE);

	req = progress_ctl_array + bid;
	req->verbose = verbose;
	ProgressDumpRequest(pid);

	OwnLatch(req->latch);
	ResetLatch(req->latch);

	SendProcSignal(pid, PROCSIG_PROGRESS, bid);
	if (debug)
		elog(LOG, "waiting on latch");

	WaitLatch(req->latch, WL_LATCH_SET | WL_TIMEOUT, PROGRESS_TIMEOUT * 1000L, WAIT_EVENT_PROGRESS);
	DisownLatch(req->latch);
	if (debug)
		elog(LOG, "finish latch wait");

	/* Fetch result and clear SHM buffer */
	if (strlen(req->buf) == 0) {
		/* We have timed out on PROGRESS_TIMEOUT */
		progress_did_timeout = 1;
		str = makeStringInfo();
		ProgressPropTextStr(str, pid, bid, 0, 0, PROP, "status", progress_backend_timeout);		
		str_len = strlen(str->data);
		memcpy(buf, str->data, str_len);
	} else {
		/* We have a result computed by the monitored backend */
		buf_len = strlen(req->buf);
		memcpy(buf, req->buf, buf_len);
	}

	if (debug)
		elog(LOG, "buf: %s", buf);

	/*
	 * Clear shm buffer
	 */
	memset(req->buf, 0, PROGRESS_AREA_SIZE);
	if (debug)
		elog(LOG, "cleared shm buf");

	/*
	 * End serialization
	 */
	LWLockRelease(ProgressLock);

	if (progress_did_timeout) {
		ereport(ERROR, (
		errcode(ERRCODE_INTERVAL_FIELD_OVERFLOW),
		errmsg("timeout to get response")));
	}
}

static
ProgressState* CreateProgressState(void)
{
	StringInfo str;
	ProgressState* prg;

	str = makeStringInfo();
 
	prg = (ProgressState*) palloc0(sizeof(ProgressState));
	prg->parallel = false;
	prg->child = false;
	prg->str = str;
	prg->indent = 0;
	prg->rtable = NULL;
	prg->plan = NULL;
	prg->pid = 0;
	prg->lineid = 0;

	return prg;
}

static
void ProgressIndent(ProgressState* ps)
{
	ps->indent++;
}

static
void ProgressUnindent(ProgressState* ps)
{
	ps->indent--;
}

/*
 * Request handling
 */
void HandleProgressSignal(void)
{
	progress_requested = true;
	InterruptPending = true;
}

void HandleProgressRequest(void)
{
	ProgressCtl* req;
	ProgressState* ps;

	MemoryContext oldcontext;
	MemoryContext progress_context;

	unsigned short running = 0;
	char* shmBufferTooShort = "shm buffer is too small";
	bool child = false;

	if (debug)
		elog(LOG, "request received");

	/*
	 * We hold interrupt here because the current SQL query could be cancelled at any time. In which 
	 * case, the current backend would not call SetLatch(). Monitoring backend would wait endlessly.
	 *
	 * To avoid such situation, a further safety measure has been added: the monitoring backend waits
	 * the response for a maximum of PROGRESS_TIMEOUT time. After this timeout has expired, the monitoring
	 * backend sends back the respponse which is empty.
	 *
	 * The current backend could indeed be interrupted before the HOLD_INTERRUPTS() is reached.
	 */
	HOLD_INTERRUPTS();

	progress_context =  AllocSetContextCreate(CurrentMemoryContext,
		"ProgressState", ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(progress_context);

	ps = CreateProgressState();
	ps->memcontext = progress_context;		// TODO: remove this useless field

	Assert(ps != NULL);
	Assert(ps->str != NULL);
	Assert(ps->str->data != NULL);

	req = progress_ctl_array + MyBackendId;
	memset(req->pid, 0, sizeof(int) * MAX_PARALLEL_WORKER_LIMIT);

	ps->verbose = req->verbose;
	ps->parallel = req->parallel;
	ps->child = req->child;
	ps->indent = req->child_indent;
	ps->disk_size = 0;
	ps->pid = getpid();
	ps->bid = MyBackendId;

	/* Reset the buffer */
	memset(req->buf, 0, PROGRESS_AREA_SIZE);

	/* Local params */
	child = req->child;

	if (debug)
		ProgressDumpRequest(getpid());

	/*
	 * Only clear previous content of ps->str
	 */
	ProgressResetReport(ps);

	if (MyQueryDesc == NULL) {
		ProgressPropText(ps, PROP, "status", "<idle backend>");
	} else if (!IsTransactionState()) {
		ProgressPropText(ps, PROP, "status", "<out of transaction>");
	} else if (MyQueryDesc->plannedstmt == NULL) {
		ProgressPropText(ps, PROP, "status", "<NULL planned statement>");
	} else if (MyQueryDesc->plannedstmt->commandType == CMD_UTILITY) {
		ProgressPropText(ps, PROP, "status", "<utility statement>");
	} else if (MyQueryDesc->already_executed == false) {
		ProgressPropText(ps, PROP, "status", "<query not yet started>");
	} else if (QueryCancelPending) {
		ProgressPropText(ps, PROP, "status", "<query cancel pending>");
	} else if (RecoveryConflictPending) {
		ProgressPropText(ps, PROP, "status", "<recovery conflict pending>");
	} else if (ProcDiePending) {
		ProgressPropText(ps, PROP, "status", "<proc die pending>");
	} else {
		running = 1;
		if (!child)
			ProgressPropText(ps, PROP, "status", "<query running>");
	}

	if (log_stmt && !child && running) {
		if (MyQueryDesc != NULL && MyQueryDesc->sourceText != NULL)
			ProgressPropText(ps, PROP, "query", MyQueryDesc->sourceText);
	}

	if (running) {
		if (!child)
			ReportTime(MyQueryDesc, ps);

		if (ps->verbose > 0) {
			ReportStack(ps);
		}

		ProgressPlan(MyQueryDesc, ps);
		if (!child && ps->verbose > VERBOSE_DISK_USE)
			ReportDisk(ps); 	/* must come after ProgressPlan() */
	}

	/* 
	 * Dump in SHM the string buffer content
	 */
	if (strlen(ps->str->data) < PROGRESS_AREA_SIZE) {
		/* Mind the '\0' char at the end of the string */
		memcpy(req->buf, ps->str->data, strlen(ps->str->data) + 1); 
	} else {
		memcpy(req->buf, shmBufferTooShort, strlen(shmBufferTooShort));
		elog(LOG, "Needed size for buffer %d", (int) strlen(ps->str->data));
	}

	/* Dump disk size used for stores, sorts, and hashes */
	req->disk_size = ps->disk_size;

	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(ps->memcontext);

	if (debug)
		elog(LOG, "setting latch");

	/* Notify of progress state delivery */
	SetLatch(req->latch);
	RESUME_INTERRUPTS();
}

static
void ProgressPlan(
	QueryDesc* query,
	ProgressState* ps)
{
	Bitmapset* rels_used = NULL;
	PlanState* planstate;

	/*
	 * Set up ProgressState fields associated with this plan tree
	 */
	Assert(query->plannedstmt != NULL);

	/* Top level tree data */
	if (query->plannedstmt != NULL)
		ps->pstmt = query->plannedstmt;

	if (query->plannedstmt->planTree != NULL)
		ps->plan = query->plannedstmt->planTree;

	if (query->planstate != NULL)
		ps->planstate = query->planstate;

	if (query->estate != NULL)
		ps->es = query->estate;

	if (query->plannedstmt->rtable != NULL)
		ps->rtable = query->plannedstmt->rtable;

	ExplainPreScanNode(query->planstate, &rels_used);

	ps->rtable_names = select_rtable_names_for_explain(ps->rtable, rels_used);
	ps->deparse_cxt = deparse_context_for_plan_rtable(ps->rtable, ps->rtable_names);
	ps->printed_subplans = NULL;

	planstate = query->planstate;
	if (IsA(planstate, GatherState) && ((Gather*) planstate->plan)->invisible) {
		planstate = outerPlanState(planstate);
	}

	if (ps->parallel && ps->child)
		ProgressNode(planstate, NIL, "child worker", NULL, ps);
	else
		ProgressNode(planstate, NIL, NULL, NULL, ps);
}
	
/*
 * This is the main workhorse for collecting query execution progress.
 *
 * planstate is the current execution state in the global execution tree
 * relationship: describes the relationship of this plan state to its parent
 * 	"outer", "inner". It is null at tol level.
 */
static
void ProgressNode(
	PlanState* planstate,
	List* ancestors,
	const char* relationship,
	const char* plan_name,
	ProgressState* ps)
{
	Plan* plan = planstate->plan;
	PlanInfo info;
	bool haschildren;
	int ret;

	if (debug)
		elog(LOG, "=> %s", nodeToString(plan));

	/*
	 * 1st step: display the node type
	 */
	ret = planNodeInfo(plan, &info);
	if (ret != 0) {
		elog(LOG, "unknown node type for plan");
	}

	ProgressPropText(ps, RELATIONSHIP, "relationship", relationship != NULL ? relationship : "progression");
	ProgressIndent(ps);

	/*
	 * Report node top properties
	 */
	if (info.pname)
		ProgressPropText(ps, NODE, "node name", info.pname);

	if (plan_name)
		ProgressPropText(ps, NODE, "plan name", plan_name);

	if (plan->parallel_aware)
		ProgressPropText(ps, PROP, "node mode", "parallel");
          
	/*
	 * Second step
	 */
	switch(nodeTag(plan)) {
	case T_SeqScan:		// ScanState
	case T_SampleScan:	// ScanState
	case T_BitmapHeapScan:	// ScanState
	case T_SubqueryScan:	// ScanState
	case T_FunctionScan:	// ScanState
	case T_ValuesScan:	// ScanState
	case T_CteScan:		// ScanState
	case T_WorkTableScan:	// ScanState
                ProgressScanRows((Scan*) plan, planstate, ps);
		ProgressScanBlks((ScanState*) planstate, ps);
                break;

	case T_TidScan:		// ScanState
		ProgressTidScan((TidScanState*) planstate, ps);
		ProgressScanBlks((ScanState*) planstate, ps);
		break;

	case T_Limit:		// PlanState
		ProgressLimit((LimitState*) planstate, ps);
		break;

	case T_ForeignScan:	// ScanState
	case T_CustomScan:	// ScanState
		ProgressCustomScan((CustomScanState*) planstate, ps);
                ProgressScanRows((Scan*) plan, planstate, ps);
		break;

	case T_IndexScan:	// ScanState
	case T_IndexOnlyScan:	// ScanState
	case T_BitmapIndexScan:	// ScanState
		ProgressScanBlks((ScanState*) planstate, ps);
		ProgressIndexScan((IndexScanState*) planstate, ps); 
		break;

	case T_ModifyTable:	// PlanState
		/*
		 * Dealt below with mt_plans array of PlanState nodes
		 */
		ProgressModifyTable((ModifyTableState *) planstate, ps);	
		break;

	case T_NestLoop:	// JoinState (includes a Planstate)
	case T_MergeJoin:	// JoinState (includes a Planstate)
		/*
		 * Does not perform long ops. Only Join
		 */
		break;

	case T_HashJoin: {	// JoinState (includes a Planstate)
		/* 
		 * uses a HashJoin with BufFile
		 */
		const char* jointype;

		switch (((Join*) plan)->jointype) {
		case JOIN_INNER:
			jointype = "Inner";
			break;

		case JOIN_LEFT:
			jointype = "Left";
			break;

		case JOIN_FULL:
			jointype = "Full";
			break;

		case JOIN_RIGHT:
			jointype = "Right";
			break;

		case JOIN_SEMI:
			jointype = "Semi";
			break;

		case JOIN_ANTI:
			jointype = "Anti";
			break;

		default:
			jointype = "???";
			break;
		}

		ProgressPropText(ps, PROP, "join type", jointype);

		}

		ProgressHashJoin((HashJoinState*) planstate, ps);
		break;

	case T_SetOp: {		// PlanState
		/*
		 *  Only uses a in memory hash table
		 */
		const char* setopcmd;

		switch (((SetOp*) plan)->cmd) {
		case SETOPCMD_INTERSECT:
			setopcmd = "Intersect";
			break;

		case SETOPCMD_INTERSECT_ALL:
			setopcmd = "Intersect All";
			break;

		case SETOPCMD_EXCEPT:
			setopcmd = "Except";
			break;

		case SETOPCMD_EXCEPT_ALL:
			setopcmd = "Except All";
			break;

		default:
			setopcmd = "???";
			break;
		}

		ProgressPropText(ps, PROP, "command", setopcmd);

		}
		break;

	case T_Sort:		// ScanState
		ProgressSort((SortState*) planstate, ps);
		break;

	case T_Material:	// ScanState
		/*
		 * Uses: ScanState and Tuplestorestate
		 */
		ProgressMaterial((MaterialState*) planstate, ps);
		ProgressScanBlks((ScanState*) planstate, ps);
		break;

	case T_Group:		// ScanState
		ProgressScanBlks((ScanState*) planstate, ps);
		break;

	case T_Agg:		// ScanState
		/* 
		 * Use tuplesortstate 2 times. Not reflected in child nodes
		 */
		ProgressAgg((AggState*) planstate, ps);
		break;

	case T_WindowAgg:	// ScanState
		/*
		 * Has a Tuplestorestate (field buffer)
		 */
		ProgressTupleStore(((WindowAggState*) plan)->buffer, ps);
		break;

	case T_Unique:		// PlanState
		/* 
		 * Does not store any tuple.
		 * Just fetch tuple and compare with previous one.
		 */
		break;

	case T_Gather:		// PlanState
		/* 
		 * Does not store any tuple.
		 * Used for parallel query
		 */
		ProgressGather((GatherState*) planstate, ps);
 		break;

	case T_GatherMerge:	// PlanState
		ProgressGatherMerge((GatherMergeState*) planstate, ps);
		break;

	case T_Hash:		// PlanState
		/* 
		 * Has a potential on file hash data
		 */
		ProgressHash((HashState*) planstate, ps);
		break;

	case T_LockRows:	// PlanState
		/*
		 * Only store tuples in memory array
		 */
		break;
	
	default:
		break;
	}

	/*
	 * Target list
	 */
        //if (ps->verbose)
        //       show_plan_tlist(planstate, ancestors, ps);

	/*
	 * Controls (sort, qual, ...) 
	 */
	//show_control_qual(planstate, ancestors, ps);

	/*
	 * Get ready to display the child plans.
	 * Pass current PlanState as head of ancestors list for children
	 */
	haschildren = ReportHasChildren(plan, planstate);
	if (haschildren) {
		ancestors = lcons(planstate, ancestors);
	}

	/*
	 * initPlan-s
	 */
	//if (planstate->initPlan) {
	//		ReportSubPlans(planstate->initPlan, ancestors, "InitPlan", ps, ProgressNode);
	//	}

	/*
	 * lefttree
	 */
	if (outerPlanState(planstate)) {
		ProgressNode(outerPlanState(planstate), ancestors, "Outer", NULL, ps);
	}

	/*
	 * righttree
	 */
	if (innerPlanState(planstate)) {
		ProgressNode(innerPlanState(planstate), ancestors, "Inner", NULL, ps);
	}

	/*
	 * special child plans
	 */
/*
	switch (nodeTag(plan)) {
	case T_ModifyTable:
		ReportMemberNodes(((ModifyTable*) plan)->plans,
			((ModifyTableState*) planstate)->mt_plans, ancestors, ps, ProgressNode);
		break;

	case T_Append:
		ReportMemberNodes(((Append*) plan)->appendplans,
			((AppendState*) planstate)->appendplans, ancestors, ps, ProgressNode);
		break;

	case T_MergeAppend:
		ReportMemberNodes(((MergeAppend*) plan)->mergeplans,
			((MergeAppendState*) planstate)->mergeplans, ancestors, ps, ProgressNode);
		break;

	case T_BitmapAnd:
		ReportMemberNodes(((BitmapAnd*) plan)->bitmapplans,
			((BitmapAndState*) planstate)->bitmapplans, ancestors, ps, ProgressNode);
		break;

	case T_BitmapOr:
		ReportMemberNodes(((BitmapOr*) plan)->bitmapplans,
			((BitmapOrState*) planstate)->bitmapplans, ancestors, ps, ProgressNode);
		break;

	case T_SubqueryScan:
		ProgressNode(((SubqueryScanState*) planstate)->subplan, ancestors,
			"Subquery", NULL, ps);
		break;

	case T_CustomScan:
		ReportCustomChildren((CustomScanState*) planstate, ancestors, ps, ProgressNode);
		break;

	default:
		break;
	}
*/

	/*
	 * subPlan-s
	 */
//	if (planstate->subPlan)
//		ReportSubPlans(planstate->subPlan, ancestors, "SubPlan", ps, ProgressNode);

	/*
	 * end of child plans
	 */
	if (haschildren)
		ancestors = list_delete_first(ancestors);

	ProgressUnindent(ps);
}

static bool
ReportHasChildren(Plan* plan, PlanState* planstate)
{
	bool haschildren;

	haschildren = planstate->initPlan || outerPlanState(planstate)
		|| innerPlanState(planstate)
		|| IsA(plan, ModifyTable)
		|| IsA(plan, Append)
		|| IsA(plan, MergeAppend)
		|| IsA(plan, BitmapAnd)
		|| IsA(plan, BitmapOr)
		|| IsA(plan, SubqueryScan)
		|| (IsA(planstate, CustomScanState) && ((CustomScanState*) planstate)->custom_ps != NIL)
		|| planstate->subPlan;

	return haschildren;
}

/**********************************************************************************
 * Indivual Progress report functions for the different execution nodes starts here.
 * These functions are leaf function of the Progress tree of functions to be called.
 *
 * For each of theses function, we need to be paranoiac because the execution tree
 * and plan tree can be in any state. Which means, that any pointer may be NULL.
 *
 * Only ProgressState data is reliable. Pointers about ProgressState data can be reference
 * without checking pointers values. All other data must be checked against NULL 
 * pointers.
 **********************************************************************************/

/*
 * Deal with worker backends
 */
static
void ProgressGather(GatherState* gs, ProgressState* ps)
{
	ParallelContext* pc;

	pc = gs->pei->pcxt;
	ProgressParallelExecInfo(pc, ps);
}

static
void ProgressGatherMerge(GatherMergeState* gms, ProgressState* ps)
{
	ParallelContext* pc;

	pc = gms->pei->pcxt;
	ProgressParallelExecInfo(pc, ps);
}

static
void ProgressParallelExecInfo(ParallelContext* pc, ProgressState* ps)
{
	ProgressCtl* req;		// Current req struct for current backend
	int pid;
	int pid_index;
	int i;

	if (debug)
		elog(LOG, "ProgressParallelExecInfo node");

	if (pc == NULL) {
		elog(LOG, "ParallelContext is NULL");
		return;
	}

	ps->parallel = true;
	ps->child = false;

	req = progress_ctl_array + MyBackendId;
	req->parallel = true;
	req->child = false;
	req->child_indent = ps->indent;

	elog(LOG, "COLLECT MASTER PARALLEL WORKER DATA");
	ProgressDumpRequest(getpid());

	/* 
	 * write pid of child worker in main req pid array
	 * note the child indentation for table indent field
	 */
	pid_index = 0;
	for (i = 0; i < pc->nworkers_launched; ++i) {
		pid = pc->worker[i].pid;
		req->pid[pid_index] = pid;
		pid_index++;
	}
}

static
void ProgressScanBlks(ScanState* ss, ProgressState* ps)
{
	HeapScanDesc hsd;
	ParallelHeapScanDesc phsd;
	unsigned int nr_blks;

	if (ss == NULL) {
		elog(LOG, "SCAN ss is null");
		return;
	}

	hsd = ss->ss_currentScanDesc;
	if (hsd == NULL) {
		elog(LOG, "SCAN hsd is null");
		return;
	}

	phsd = hsd->rs_parallel;
	if (phsd != NULL) {
		/* Parallel query */
		ProgressPropText(ps, PROP, "scan mode", "parallel");
		if (phsd->phs_nblocks != 0 && phsd->phs_cblock != InvalidBlockNumber) {
			if (phsd->phs_cblock > phsd->phs_startblock)
				nr_blks = phsd->phs_cblock - phsd->phs_startblock;
			else
				nr_blks = phsd->phs_cblock + phsd->phs_nblocks - phsd->phs_startblock;

			ProgressPropLong(ps, PROP, "fetched", nr_blks, BLK_UNIT);
			ProgressPropLong(ps, PROP, "total", phsd->phs_nblocks, BLK_UNIT);
			ProgressPropLong(ps, PROP, "completion", 100 * nr_blks/(phsd->phs_nblocks), PERCENT_UNIT);
		} else {
			if (phsd->phs_nblocks != 0)
				ProgressPropLong(ps, PROP, "total", phsd->phs_nblocks, BLK_UNIT);

			ProgressPropLong(ps, PROP, "completion", 100, PERCENT_UNIT);
		}
	} else {
		/* Not a parallel query */
		if (hsd->rs_nblocks != 0 && hsd->rs_cblock != InvalidBlockNumber) {
			if (hsd->rs_cblock > hsd->rs_startblock)
				nr_blks = hsd->rs_cblock - hsd->rs_startblock;
			else
				nr_blks = hsd->rs_cblock + hsd->rs_nblocks - hsd->rs_startblock;

	
			ProgressPropLong(ps, PROP, "fetched", nr_blks, BLK_UNIT);
			ProgressPropLong(ps, PROP, "total", hsd->rs_nblocks, BLK_UNIT);
			ProgressPropLong(ps, PROP, "completion BLOCKS", 100 * nr_blks/(hsd->rs_nblocks), PERCENT_UNIT);
		} else {
			if (hsd->rs_nblocks != 0)
				ProgressPropLong(ps, PROP, "total", hsd->rs_nblocks, BLK_UNIT);

			ProgressPropLong(ps, PROP, "completion", 100, PERCENT_UNIT);
		}
	}
}

static 
void ProgressScanRows(Scan* plan, PlanState* planstate, ProgressState* ps)
{
	Index rti;
	RangeTblEntry* rte;
	char* objectname;

	if (plan == NULL)
		return;

	if (planstate == NULL)
		return;

	rti = plan->scanrelid; 
	rte = rt_fetch(rti, ps->rtable);
	objectname = get_rel_name(rte->relid);

	if (objectname != NULL) {
		ProgressPropText(ps, PROP, "scan on", quote_identifier(objectname));
	}

	if (ps->verbose >= VERBOSE_ROW_SCAN) {	
		ProgressPropLong(ps, PROP, "fetched",
			(unsigned long) planstate->plan_rows, ROW_UNIT);
		ProgressPropLong(ps, PROP, "total",
			(unsigned long) plan->plan.plan_rows, ROW_UNIT);
		ProgressPropLong(ps, PROP, "completion",
			(unsigned short) planstate->percent_done, PERCENT_UNIT);
	}
}

static
void ProgressTidScan(TidScanState* ts, ProgressState* ps)
{
	unsigned int percent;

	if (ts == NULL) {
		return;
	}

	if (ts->tss_NumTids == 0)
		percent = 0;
	else 
		percent = (unsigned short)(100 * (ts->tss_TidPtr) / (ts->tss_NumTids));

	ProgressPropLong(ps, PROP, "fetched", (long int) ts->tss_TidPtr, ROW_UNIT);
	ProgressPropLong(ps, PROP, "total", (long int) ts->tss_NumTids, ROW_UNIT);
	ProgressPropLong(ps, PROP, "completion", percent, PERCENT_UNIT);
}

static
void ProgressLimit(LimitState* ls, ProgressState* ps)
{
	if (ls == NULL)
		return;

	if (ls->position == 0) {
		ProgressPropLong(ps, PROP, "offset", 0, PERCENT_UNIT);
		ProgressPropLong(ps, PROP, "count", 0, PERCENT_UNIT);
	}

	if (ls->position > 0 && ls->position <= ls->offset) {
		ProgressPropLong(ps, PROP, "offset",
			(unsigned short)(100 * (ls->position)/(ls->offset)), PERCENT_UNIT);
		ProgressPropLong(ps, PROP, "count", 0, PERCENT_UNIT);
	}

	if (ls->position > ls->offset) {
		ProgressPropLong(ps, PROP, "offset", 100, PERCENT_UNIT);
		ProgressPropLong(ps, PROP, "count",
			(unsigned short)(100 * (ls->position - ls->offset)/(ls->count)), PERCENT_UNIT);
	}
}

static
void ProgressCustomScan(CustomScanState* cs, ProgressState* ps)
{
	if (cs == NULL)
		return;

//	if (cs->methods->ProgressCustomScan) {
//		cs->methods->ProgressCustomScan(cs, NULL, ps);
//	}
}

static
void ProgressIndexScan(IndexScanState* is, ProgressState* ps) 
{
	PlanState planstate;
	Plan* p;

	if (is == NULL) {
		return;
	}

	planstate = is->ss.ps;
	p = planstate.plan;
	if (p == NULL) {
		return;
	}

	if (ps->verbose > VERBOSE_ROW_SCAN) {
		ProgressPropLong(ps, PROP, "fetched", (long int) planstate.plan_rows, ROW_UNIT);
		ProgressPropLong(ps, PROP, "total", (long int) p->plan_rows, ROW_UNIT);
	}

	ProgressPropLong(ps, PROP, "completion", (unsigned short) planstate.percent_done, PERCENT_UNIT);
}

static
void ProgressModifyTable(ModifyTableState *mts, ProgressState* ps)
{
	EState* es;

	if (mts == NULL)
		return;

	es = mts->ps.state;
	if (es == NULL)
		return;

	ProgressPropLong(ps, PROP, "modified", (long int) es->es_processed, ROW_UNIT);
}

static
void ProgressHash(HashState* hs, ProgressState* ps)
{
	if (hs == NULL)
		return;
	
	ProgressHashJoinTable((HashJoinTable) hs->hashtable, ps);
}

static
void ProgressHashJoin(HashJoinState* hjs, ProgressState* ps)
{
	if (hjs == NULL)
		return;

	ProgressHashJoinTable((HashJoinTable) hjs->hj_HashTable, ps);
}

/*
 * HashJoinTable is not a node type
 */
static
void ProgressHashJoinTable(HashJoinTable hashtable, ProgressState* ps)
{
	int i;
	unsigned long reads;
	unsigned long writes;
	unsigned long disk_size;
	unsigned long lreads;
	unsigned long lwrites;
	unsigned long ldisk_size;

	/*
	 * Could be used but not yet allocated
	 */
	if (hashtable == NULL)
		return;
		
	if (hashtable->nbatch <= 1)
		return;

	if (ps->verbose >= VERBOSE_HASH_JOIN)
		ProgressPropLong(ps, PROP, "hashtable nbatch", hashtable->nbatch, "");

	/*
	 * Display global reads and writes
	 */
	reads = 0;
	writes = 0;
	disk_size = 0;

	for (i = 0; i < hashtable->nbatch; i++) {
		if (hashtable->innerBatchFile[i]) {
			ProgressBufFileRW(hashtable->innerBatchFile[i],
				ps, &lreads, &lwrites, &ldisk_size);
			reads += lreads;
			writes += lwrites;
			disk_size += ldisk_size;
		}

		if (hashtable->outerBatchFile[i]) {
			ProgressBufFileRW(hashtable->outerBatchFile[i],
				ps, &lreads, &lwrites, &ldisk_size);
			reads += lreads;
			writes += lwrites;
			disk_size += ldisk_size;
		}
	}

	/* 
	 * Update SQL query wide disk use
  	 */
	ps->disk_size += disk_size;

	if (ps->verbose >= VERBOSE_HASH_JOIN) {
		ProgressPropLong(ps, PROP, "read", reads/1024, KBYTE_UNIT);
		ProgressPropLong(ps, PROP, "write", writes/1024, KBYTE_UNIT);
	}

	if (writes > 0)
		ProgressPropLong(ps, PROP, "completion", reads/writes, PERCENT_UNIT);

	if (ps->verbose >= VERBOSE_DISK_USE)
		ProgressPropLong(ps, PROP, "disk used", disk_size/1024, KBYTE_UNIT);

	/*
	 * Only display details if requested
	 */ 
	if (ps->verbose < VERBOSE_HASH_JOIN_DETAILED)
		return;

	if (hashtable->nbatch == 0)
		return;

	ps->indent++;
	for (i = 0; i < hashtable->nbatch; i++) {
		ProgressPropLong(ps, PROP, "batch", (long int) i, "");

		if (hashtable->innerBatchFile[i]) {
			ps->indent++;
			ProgressPropText(ps, PROP, "group", "inner");
			ProgressBufFile(hashtable->innerBatchFile[i], ps);
			ps->indent--;
		}

		if (hashtable->outerBatchFile[i]) {
			ps->indent++;
			ProgressPropText(ps, PROP, "group", "outer");
			ProgressBufFile(hashtable->outerBatchFile[i], ps);
			ps->indent--;
		}
	}

	ps->indent--;
}

static
void ProgressBufFileRW(BufFile* bf, ProgressState* ps,
	unsigned long* reads, unsigned long* writes, unsigned long* disk_size)
{
	MemoryContext oldcontext;
	struct buffile_state* bfs;
	int i;

	if (bf == NULL)
		return;

	*reads = 0;
	*writes = 0;
	*disk_size = 0;

	oldcontext = MemoryContextSwitchTo(ps->memcontext);
	bfs = BufFileState(bf);
	MemoryContextSwitchTo(oldcontext);

	*disk_size = bfs->disk_size;	

	for (i = 0; i < bfs->numFiles; i++) {
		*reads += bfs->bytes_read[i];
		*writes += bfs->bytes_write[i];
	}
}
	
static
void ProgressBufFile(BufFile* bf, ProgressState* ps)
{
	int i;
	struct buffile_state* bfs;
	MemoryContext oldcontext;
	
	if (bf == NULL)
		return;

        oldcontext = MemoryContextSwitchTo(ps->memcontext);
	bfs = BufFileState(bf);
	MemoryContextSwitchTo(oldcontext);

	if (ps->verbose < VERBOSE_BUFFILE)
		return;

	ps->indent++;
	ProgressPropLong(ps, PROP, "buffile nr files", bfs->numFiles, "");

	if (bfs->numFiles == 0)
		return;

	if (ps->verbose >= VERBOSE_DISK_USE)
		ProgressPropLong(ps, PROP, "disk used", bfs->disk_size/1024,  KBYTE_UNIT);

	for (i = 0; i < bfs->numFiles; i++) {
		ps->indent++;
		ProgressPropLong(ps, NODE, "file", i, "");
		ProgressPropLong(ps, PROP, "read", bfs->bytes_read[i]/1024, KBYTE_UNIT);
		ProgressPropLong(ps, PROP, "write", bfs->bytes_write[i]/1024, KBYTE_UNIT);
		ps->indent--;
	}	

	ps->indent--;
}

static
void ProgressMaterial(MaterialState* planstate, ProgressState* ps)
{
	Tuplestorestate* tss;

	if (planstate == NULL)
		return;

	tss = planstate->tuplestorestate;
	ProgressTupleStore(tss, ps);

}
/*
 * Tuplestorestate is not a node type
 */
static
void ProgressTupleStore(Tuplestorestate* tss, ProgressState* ps)
{
	struct tss_report tssr;

	if (tss == NULL)
		return;

	tuplestore_get_state(tss, &tssr);

	switch (tssr.status) {
	case TSS_INMEM:
		/* Add separator */
		ProgressPropLong(ps, PROP, "memory write", (long int) tssr.memtupcount, ROW_UNIT);
		if (tssr.memtupskipped > 0)
			ProgressPropLong(ps, PROP, "memory skipped", (long int) tssr.memtupskipped, ROW_UNIT);

		ProgressPropLong(ps, PROP, "memory read", (long int) tssr.memtupread, ROW_UNIT);
		if (tssr.memtupdeleted)
			ProgressPropLong(ps, PROP, "memory deleted", (long int) tssr.memtupread, ROW_UNIT);
		break;
	
	case TSS_WRITEFILE:
	case TSS_READFILE:
		if (tssr.status == TSS_WRITEFILE)
			ProgressPropText(ps, PROP, "file store", "write");
		else 
			ProgressPropText(ps, PROP, "file store", "read");

		ProgressPropLong(ps, PROP, "readptrcount", tssr.readptrcount, "");
		ProgressPropLong(ps, PROP, "write", (long int ) tssr.tuples_count, ROW_UNIT);
		if (tssr.tuples_skipped)
			ProgressPropLong(ps, PROP, "skipped", (long int) tssr.tuples_skipped, ROW_UNIT);

		ProgressPropLong(ps, PROP, "read", (long int) tssr.tuples_read, ROW_UNIT);
		if (tssr.tuples_deleted)
			ProgressPropLong(ps, PROP, "deleted", (long int) tssr.tuples_deleted, ROW_UNIT);

		ps->disk_size += tssr.disk_size;
		if (ps->verbose >= VERBOSE_DISK_USE)
			ProgressPropLong(ps, PROP, "disk used", tssr.disk_size/2014, KBYTE_UNIT);
		break;

	default:
		break;
	}
}

static
void ProgressAgg(AggState* planstate, ProgressState* ps)
{
	if (planstate == NULL)
		return;

	ProgressTupleSort(planstate->sort_in, ps);
	ProgressTupleSort(planstate->sort_out, ps);
}

static
void ProgressSort(SortState* ss, ProgressState* ps)
{
	Assert(nodeTag(ss) == T_SortState);

	if (ss == NULL)
		return;

	if (ss->tuplesortstate == NULL)
		return;

	ProgressTupleSort(ss->tuplesortstate, ps);
}

static
void ProgressTupleSort(Tuplesortstate* tss, ProgressState* ps)
{
	struct ts_report* tsr;
	MemoryContext oldcontext;
	char status[] = "sort status";
	
	if (tss == NULL)
		return;
	
	oldcontext = MemoryContextSwitchTo(ps->memcontext);
	tsr = tuplesort_get_state(tss);
	MemoryContextSwitchTo(oldcontext);

	switch (tsr->status) {
	case TSS_INITIAL:		/* Loading tuples in mem still within memory limit */
	case TSS_BOUNDED:		/* Loading tuples in mem into bounded-size heap */
		ProgressPropText(ps, PROP, status, "loading tuples in memory");
		ProgressPropLong(ps, PROP, "tuples in memory", tsr->memtupcount, ROW_UNIT);	
		break;

	case TSS_SORTEDINMEM:		/* Sort completed entirely in memory */
		ProgressPropText(ps, PROP, status, "sort completed in memory");
		ProgressPropLong(ps, PROP, "tuples in memory", tsr->memtupcount, ROW_UNIT);	
		break;

	case TSS_BUILDRUNS:		/* Dumping tuples to tape */
		switch (tsr->sub_status) {
		case TSSS_INIT_TAPES:
			ProgressPropText(ps, PROP, status, "on tapes initializing");
			break;

		case TSSS_DUMPING_TUPLES:
			ProgressPropText(ps, PROP, status, "on tapes writing");
			break;

		case TSSS_SORTING_ON_TAPES:
			ProgressPropText(ps, PROP, status, "on tapes sorting");
			break;

		case TSSS_MERGING_TAPES:
			ProgressPropText(ps, PROP, status, "on tapes merging");
			break;
		default:
			;
		};

		dumpTapes(tsr, ps);
		break;
	
	case TSS_FINALMERGE: 		/* Performing final merge on-the-fly */
		ProgressPropText(ps, PROP, status, "on tapes final merge");
		dumpTapes(tsr, ps);	
		break;

	case TSS_SORTEDONTAPE:		/* Sort completed, final run is on tape */
		switch (tsr->sub_status) {
		case TSSS_FETCHING_FROM_TAPES:
			ProgressPropText(ps, PROP, status, "fetching from sorted tapes");
			break;

		case TSSS_FETCHING_FROM_TAPES_WITH_MERGE:
			ProgressPropText(ps, PROP, status, "fetching from sorted tapes with merge");
			break;
		default:
			;
		};

		dumpTapes(tsr, ps);	
		break;

	default:
		ProgressPropText(ps, PROP, status, "unexpected sort state");
	};
}

static
void dumpTapes(struct ts_report* tsr, ProgressState* ps)
{
	int i;
	int percent_effective;

	if (tsr == NULL)
		return;

	if (tsr->tp_write_effective > 0) {
		percent_effective = 100 * (tsr->tp_read_effective)/(tsr->tp_write_effective);
	} else {
		percent_effective = 0;
	}

	if (ps->verbose >= VERBOSE_TAPES) {
		ProgressPropLong(ps, PROP, "merge reads", tsr->tp_read_merge, ROW_UNIT);
		ProgressPropLong(ps, PROP, "merge writes", tsr->tp_write_merge, ROW_UNIT);
		ProgressPropLong(ps, PROP, "effective reads", tsr->tp_read_effective, ROW_UNIT);
		ProgressPropLong(ps, PROP, "effective writes", tsr->tp_write_effective, ROW_UNIT);
	}

	ProgressPropLong(ps, PROP, "completion", percent_effective, PERCENT_UNIT);
	if (ps->verbose >= VERBOSE_DISK_USE)
		ProgressPropLong(ps, PROP, "tape size", tsr->blocks_alloc, BLK_UNIT);

	/*
	 * Update total disk size used 
	 */
	ps->disk_size += tsr->blocks_alloc * BLCKSZ;

	if (ps->verbose < VERBOSE_TAPES_DETAILED)
		return;

	/*
	 * Verbose report
	 */
	ProgressPropLong(ps, PROP, "tapes total", tsr->maxTapes, NO_UNIT);
	ProgressPropLong(ps, PROP, "tapes actives", tsr->activeTapes, NO_UNIT);

	if (tsr->result_tape != -1)
		ProgressPropLong(ps, PROP, "tape result", tsr->result_tape, NO_UNIT);

	if (tsr->maxTapes != 0) {
		for (i = 0; i< tsr->maxTapes; i++) {
			ps->indent++;
			ProgressPropLong(ps, NODE, "tape idx", i, NO_UNIT);

			ps->indent++;
			if (tsr->tp_fib != NULL)
				ProgressPropLong(ps, PROP, "fib", tsr->tp_fib[i], NO_UNIT);

			if (tsr->tp_runs != NULL)
				ProgressPropLong(ps, PROP, "runs", tsr->tp_runs[i], NO_UNIT);

			if (tsr->tp_dummy != NULL)
				ProgressPropLong(ps, PROP, "dummy", tsr->tp_dummy[i], NO_UNIT);

			if (tsr->tp_read != NULL)
				ProgressPropLong(ps, PROP, "read", tsr->tp_read[i], ROW_UNIT);

			if (tsr->tp_write)	
				ProgressPropLong(ps, PROP, "write", tsr->tp_write[i], ROW_UNIT);
				
			ps->indent--;
			ps->indent--;
		}
	}
}

static
void ReportTime(QueryDesc* query, ProgressState* ps)
{
	instr_time currenttime;

	if (query == NULL)
		return;

	if (query->totaltime == NULL)
		return;

	INSTR_TIME_SET_CURRENT(currenttime);
	INSTR_TIME_SUBTRACT(currenttime, query->totaltime->starttime);

	if (ps->verbose >= VERBOSE_TIME_REPORT) {
		ProgressPropLong(ps, PROP, "time used",
			INSTR_TIME_GET_MILLISEC(currenttime)/1000, SECOND_UNIT);
	}
}

static  
void ReportStack(ProgressState* ps)
{
	unsigned long depth;
	unsigned long max_depth;

	depth =	get_stack_depth();
	max_depth = get_max_stack_depth();

	if (ps->verbose >= VERBOSE_STACK) {
		ProgressPropLong(ps, PROP, "stack depth", depth, BYTE_UNIT);
		ProgressPropLong(ps, PROP, "max stack depth", max_depth, BYTE_UNIT);
	}
}

static
void ReportDisk(ProgressState*  ps)
{
	unsigned long size;
	char* unit;

	size = ps->disk_size;
	
	if (size < 1024) {
		unit = BYTE_UNIT;
	} else if (size >= 1024 && size < 1024 * 1024) {
		unit = KBYTE_UNIT;
		size = size / 1024;
	} else if (size >= 1024 * 1024 && size < 1024 * 1024 * 1024) {
		unit = MBYTE_UNIT;
		size = size / (1024 * 1024);
	} else {
		unit = GBYTE_UNIT;
		size = size / (1024 * 1024 * 1024);
	}
	
	if (ps->verbose >= VERBOSE_DISK_USE) 	
		ProgressPropLong(ps, PROP, "disk used", size, unit);
}
