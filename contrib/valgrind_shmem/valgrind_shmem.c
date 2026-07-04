#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/shm_toc.h"
#include "storage/shmem.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC_EXT(
					.name = "valgrind_shmem",
					.version = PG_VERSION
);

#define	PG_TEST_VALGRIND_SHMEM_MAGIC 0x74686f72

PG_FUNCTION_INFO_V1(valgrind_shmem_main_get);
PG_FUNCTION_INFO_V1(valgrind_shmem_shm_toc_fg_get);
PG_FUNCTION_INFO_V1(valgrind_shmem_shm_toc_bg_get);

pg_noreturn extern PGDLLEXPORT void valgrind_shmem_main(Datum main_arg);

typedef struct
{
	int32		index;
	bool		done;
	int32		result;
} WorkerHeader;

typedef struct
{
	int32		a[10];
} ShmemState;

static ShmemState *state;
static int we_valdgrind_shmem_request = 0;

static void
shmem_request(void *arg)
{
	ShmemRequestStruct(.name = "valgrind_shmem: state",
					   .size = sizeof(ShmemState),
					   .ptr = (void **) &state,
		);
}

static void
shmem_init(void *arg)
{
	for (int i = 0; i < 10; i++)
		state->a[i] = i * 2;
}

static const ShmemCallbacks shmem_callbacks = {
	.request_fn = shmem_request,
	.init_fn = shmem_init,
};

/*
 * Gets an array element from a struct in the main shared memory.
 */
Datum
valgrind_shmem_main_get(PG_FUNCTION_ARGS)
{
	int32		index = PG_GETARG_INT32(0);

	PG_RETURN_INT32(state->a[index]);
}

/*
 * Gets an array element from a struct in a shm_toc entry in DSM.
 */
Datum
valgrind_shmem_shm_toc_fg_get(PG_FUNCTION_ARGS)
{
	int32		index = PG_GETARG_INT32(0);
	shm_toc_estimator e;
	Size		segsize;
	dsm_segment *seg;
	shm_toc    *toc;
	ShmemState *state;
	int32		ret;

	shm_toc_initialize_estimator(&e);
	shm_toc_estimate_chunk(&e, sizeof(ShmemState));
	shm_toc_estimate_keys(&e, 1);
	segsize = shm_toc_estimate(&e);

	seg = dsm_create(shm_toc_estimate(&e), 0);
	toc = shm_toc_create(PG_TEST_VALGRIND_SHMEM_MAGIC, dsm_segment_address(seg), segsize);

	state = shm_toc_allocate(toc, sizeof(ShmemState));
	for (int i = 0; i < 10; i++)
		state->a[i] = i * 3;
	shm_toc_insert(toc, 1, state);

	ret = state->a[index];

	dsm_detach(seg);

	PG_RETURN_INT32(ret);
}

/*
 * Gets an array element from a struct in a shm_toc entry in DSM in a
 * background worker.
 */
Datum
valgrind_shmem_shm_toc_bg_get(PG_FUNCTION_ARGS)
{
	int32		index = PG_GETARG_INT32(0);
	shm_toc_estimator e;
	Size		segsize;
	dsm_segment *seg;
	shm_toc    *toc;
	WorkerHeader *header;
	ShmemState *state;
	BackgroundWorker worker = {
		.bgw_flags = BGWORKER_SHMEM_ACCESS,
		.bgw_start_time = BgWorkerStart_ConsistentState,
		.bgw_restart_time = BGW_NEVER_RESTART,
		.bgw_library_name = "valgrind_shmem",
		.bgw_function_name = "valgrind_shmem_main",
		.bgw_type = "valgrind_shmem",
		.bgw_name = "valgrind_shmem worker",
		.bgw_notify_pid = MyProcPid
	};
	BackgroundWorkerHandle *whandle;
	int32		ret;

	shm_toc_initialize_estimator(&e);
	shm_toc_estimate_chunk(&e, sizeof(WorkerHeader));
	shm_toc_estimate_chunk(&e, sizeof(ShmemState));
	shm_toc_estimate_keys(&e, 2);
	segsize = shm_toc_estimate(&e);

	seg = dsm_create(shm_toc_estimate(&e), 0);
	toc = shm_toc_create(PG_TEST_VALGRIND_SHMEM_MAGIC, dsm_segment_address(seg), segsize);

	header = shm_toc_allocate(toc, sizeof(WorkerHeader));
	header->index = index;
	header->done = false;
	shm_toc_insert(toc, 0, header);

	state = shm_toc_allocate(toc, sizeof(ShmemState));
	for (int i = 0; i < 10; i++)
		state->a[i] = i * 3;
	shm_toc_insert(toc, 1, state);

	worker.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(seg));

	if (!RegisterDynamicBackgroundWorker(&worker, &whandle))
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				errmsg("could not register background process"),
				errhint("You may need to increase \"max_worker_processes\"."));

	for (;;)
	{
		BgwHandleStatus status;
		pid_t		pid;

		if (header->done)
			break;

		status = GetBackgroundWorkerPid(whandle, &pid);
		if (status == BGWH_STOPPED || status == BGWH_POSTMASTER_DIED)
			ereport(ERROR,
				errmsg("background worker died unexpectedly"));

		if (we_valdgrind_shmem_request == 0)
			we_valdgrind_shmem_request = WaitEventExtensionNew("TestValgrindHsmemRequest");

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
						 we_valdgrind_shmem_request);

		ResetLatch(MyLatch);

		CHECK_FOR_INTERRUPTS();
	}

	ret = header->result;

	dsm_detach(seg);

	PG_RETURN_INT32(ret);
}

void
valgrind_shmem_main(Datum main_arg)
{
	dsm_segment *seg;
	shm_toc    *toc;
	WorkerHeader *header;
	ShmemState *state;

	seg = dsm_attach(DatumGetUInt32(main_arg));
	if (seg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("unable to map dynamic shared memory segment")));

	toc = shm_toc_attach(PG_TEST_VALGRIND_SHMEM_MAGIC, dsm_segment_address(seg));
	if (toc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("bad magic number in dynamic shared memory segment")));

	header = shm_toc_lookup(toc, 0, false);
	state = shm_toc_lookup(toc, 1, false);

	pg_usleep(500 * 1000);

	header->result = state->a[header->index];
	header->done = true;

	proc_exit(0);
}

void
_PG_init(void)
{
	RegisterShmemCallbacks(&shmem_callbacks);
}
