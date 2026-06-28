/*-------------------------------------------------------------------------
 *
 * backend_msg.c
 *	  Shared memory region for passing messages to backend processes.
 *
 * When pg_terminate_backend() or pg_cancel_backend() is called with a
 * non-empty message, the signaling backend writes the message into the
 * target's BackendMsgSlot before delivering the signal.  The target reads
 * it in ProcessInterrupts() and includes it as errdetail in the FATAL/ERROR.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/backend_msg.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/backend_msg.h"

typedef struct
{
	pid_t		pid;
	slock_t		lock;
	char		msg[BACKEND_MSG_MAX_LEN];
} BackendMsgSlot;


static BackendMsgSlot *BackendMsgSlots;
static BackendMsgSlot *MyBackendMsgSlot;

static void
backend_msg_slot_clean(int code, Datum arg)
{
	Assert(MyBackendMsgSlot != NULL);

	SpinLockAcquire(&MyBackendMsgSlot->lock);

	MyBackendMsgSlot->msg[0] = '\0';
	MyBackendMsgSlot->pid = 0;

	SpinLockRelease(&MyBackendMsgSlot->lock);

	MyBackendMsgSlot = NULL;
}

/*
 * BackendMsgShmemInit
 *		Allocate and initialize the BackendMsgSlots shared memory array.
 *		Called once by the postmaster at startup.
 */
void
BackendMsgShmemInit(void)
{
	Size		size;
	bool		found;

	size = BackendMsgShmemSize();
	BackendMsgSlots = ShmemInitStruct("BackendMsgSlots", size, &found);

	if (found)
		return;

	memset(BackendMsgSlots, 0, size);

	for (int i = 0; i < MaxBackends; ++i)
		SpinLockInit(&BackendMsgSlots[i].lock);
}

/*
 * BackendMsgShmemSize
 *		Compute the shared memory size required for BackendMsgSlots.
 */
Size
BackendMsgShmemSize(void)
{
	return mul_size(MaxBackends, sizeof(BackendMsgSlot));
}

/*
 * BackendMsgInit
 *		Initialize the slot for the current backend.  Must be called once
 *		per backend after MyProcPid and MyProcNumber are set.
 */
void
BackendMsgInit(int id)
{
	BackendMsgSlot *slot;

	slot = &BackendMsgSlots[id];

	slot->msg[0] = '\0';
	slot->pid = MyProcPid;

	MyBackendMsgSlot = slot;

	on_shmem_exit(backend_msg_slot_clean, Int32GetDatum(0) /* not used */ );
}

/*
 * Write msg into the slot for the backend identified by (procno, pid).
 *
 * We index directly by procno (O(1)) but verify slot->pid under the spinlock
 * to guard against the slot being reused by a new backend after the target
 * exited.
 *
 * Returns the number of bytes written, 0 if msg is empty, or -1 if the slot
 * is no longer owned by the expected pid.
 */
int
BackendMsgSet(ProcNumber procno, pid_t pid, const char *msg)
{
	BackendMsgSlot *slot;
	int			len;

	if (msg == NULL || msg[0] == '\0')
		return 0;

	slot = &BackendMsgSlots[procno];

	SpinLockAcquire(&slot->lock);

	if (slot->pid != pid)
	{
		SpinLockRelease(&slot->lock);
		ereport(DEBUG1,
				(errmsg("could not set message for backend with PID %d: no longer exists",
						pid)));
		return -1;
	}

	len = pg_mbcliplen(msg, strlen(msg), sizeof(slot->msg) - 1);
	memcpy(slot->msg, msg, len);
	slot->msg[len] = '\0';

	SpinLockRelease(&slot->lock);

	return len;
}

/*
 * BackendMsgGet
 *		Copy the pending message (if any) into buf and clear the slot.
 *		Returns the number of bytes copied.  Called by the target backend
 *		in ProcessInterrupts() before issuing the FATAL/ERROR.
 */
int
BackendMsgGet(char *buf, int max_len)
{
	int			len;

	if (MyBackendMsgSlot == NULL)
		return 0;

	SpinLockAcquire(&MyBackendMsgSlot->lock);

	len = strlcpy(buf, MyBackendMsgSlot->msg, max_len);
	memset(MyBackendMsgSlot->msg, '\0', sizeof(MyBackendMsgSlot->msg));

	SpinLockRelease(&MyBackendMsgSlot->lock);

	return len;
}

/*
 * BackendMsgIsSet
 *		Return true if a non-empty message is waiting in the current
 *		backend's slot.
 */
bool
BackendMsgIsSet(void)
{
	bool		result = false;

	if (MyBackendMsgSlot == NULL)
		return false;

	SpinLockAcquire(&MyBackendMsgSlot->lock);
	result = MyBackendMsgSlot->msg[0] != '\0';
	SpinLockRelease(&MyBackendMsgSlot->lock);

	return result;
}
