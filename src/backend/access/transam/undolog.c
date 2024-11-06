/*-------------------------------------------------------------------------
 *
 * undolog.c
 *		Undo log manager for PostgreSQL
 *
 * This module logs the cleanup procedures required during a transaction abort.
 * The information is recorded in WAL-logged files to ensure post-crash
 * recovery runs the necessary cleanup procedures.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/undolog.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>

#include "lib/stringinfo.h"
#include "access/parallel.h"
#include "access/undolog.h"
#include "access/twophase_rmgr.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "lib/dshash.h"
#include "catalog/storage_ulog.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/procarray.h"
#include "utils/memutils.h"


#define ULOG_FILE_MAGIC 0x474f4c55	/* 'ULOG' in big-endian */

/* Resource manager definition */
typedef struct RmgrUndoData
{
	const char *rm_name;
	void	(*rm_undo) (UndoLogRecord *record, ULogContext cxt, bool redo,
						bool crashed);
	void	(*rm_undo_event) (ULogEvent event);
} RmgrUndoData;

#define PG_RMGR(symname,name,redo,desc,identify,startup,cleanup,mask,decode,undo,undo_desc,undo_identify,undo_event) \
	{ name, undo, undo_event },

static RmgrUndoData	RmgrUndo[RM_MAX_ID + 1] = {
#include "access/rmgrlist.h"
};
#undef PG_RMGR

/*
 * Undo log shared data.
 *
 * Undo log records are first stored in a fixed number of fixed-length slots,
 * each placed in shared memory and keyed by top transaction IDs. When a slot
 * becomes full, its data is flushed to a file named after the full transaction
 * ID, and the slot then stores the subsequent part. During each checkpoint,
 * all slots are flushed to disk then emptied. If a new transaction arrives and
 * no slot is available, the slot with the oldest transaction ID is
 * evicted. Although the number of slots and the buffer length could be made
 * configurable, they are currently fixed.
 */
#define ULOG_SLOT_NUM	    32
#define ULOG_SLOT_BUF_LEN 1024

typedef struct UndoLogSlot
{
	LWLock				lock;
	FullTransactionId	xid;
	off_t				off;
	int					len;
	uint8				buf[ULOG_SLOT_BUF_LEN];
} UndoLogSlot;

/* struct for data stored in shared memory */
typedef struct ULogSharedData
{
	UndoLogSlot			slots[ULOG_SLOT_NUM];
} ULogSharedData;

/*
 * Struct for top-level management local variables.
 *
 * Stored in local memory. current_slot is the ID of the slot (described later)
 * that this backend currently considers itself to be using. current_xid is the
 * transaction ID that the slot should hold. Since another process can steal a
 * slot under use, current_xid is used to verify if the slot pointed by
 * current_slot is indeed the one this process is using. buf holds a memory
 * area of length buflen for various purposes in this module.
 */
typedef struct ULogLocalData
{
	MemoryContext		cxt;			/* working memroy context */
	int					current_slot;	/* slot id currently used by me */
	FullTransactionId	current_xid;	/* the current xid */
	ULogSharedData	   *shared_area;	/* shared memory area */
	void			   *buf;			/* working buffer */
	int					buflen;			/* length of the buffer */
} ULogLocalData;

static ULogLocalData ULogLocal;

/* short cut macros */
#define UndoLogContext (ULogLocal.cxt)
#define ULogShared	(ULogLocal.shared_area)

/*
 * Shared memory intializer functions
 */
Size
UndoLogShmemSize(void)
{
	return MAXALIGN(sizeof(ULogSharedData));
}

void
UndoLogShmemInit(void)
{
	bool found;

	ULogShared = (ULogSharedData *) ShmemInitStruct("UNDO Log Data",
														UndoLogShmemSize(),
														&found);
	if (!found)
	{
		/* initialize all slots */
		for (int i = 0 ; i < ULOG_SLOT_NUM ; i++)
		{
			UndoLogSlot *slot = &ULogShared->slots[i];
			slot->xid = InvalidFullTransactionId;
			LWLockInitialize(&slot->lock, LWTRANCHE_UNDOLOG_DATA);
		}
	}
}

/*
 * InitUndoLog() - initialize undo log system
 */
void
InitUndoLog(void)
{
	/* shouldn't be called from postmaster */
	Assert(IsUnderPostmaster || !IsPostmasterEnvironment);

	ULogLocal.cxt = AllocSetContextCreate(TopMemoryContext,
										  "Undo log system",
										  ALLOCSET_DEFAULT_SIZES);
	ULogLocal.current_slot = -1;
	ULogLocal.current_xid = InvalidFullTransactionId;

	/*
	 * While this buffer could be made flexible in size, a fixed-size buffer is
	 * allocated to avoid pallocs within critical sections.
	 */
	ULogLocal.buflen = 1024;
	ULogLocal.buf = MemoryContextAlloc(UndoLogContext, ULogLocal.buflen);
}

/*
 * undolog_ensure_buffer()
 *
 * Ensures that the data buffer in ULogLocal is larger than the specified size.
 */
static void *
undolog_ensure_buffer(Size size)
{
	/*
	 * The buffer is not reallocated for the reasons mentioned above.  It is
	 * unlikely that the current buffer size will be insufficient, but a
	 * mechanism to determine the maximum required buffer size for the entire
	 * system in advance may be necessary.
	 */
	Assert (size <= ULogLocal.buflen);

	return ULogLocal.buf;
}

/*
 * undolog_file_exists()
 *
 * Checks for the existence of the file corresponding to the specified xid.
 */
static bool
undolog_file_exists(FullTransactionId xid)
{
	char fname[MAXPGPATH];
	struct stat statbuf;

	UndoLogSetFilename(fname, xid);

	if (stat(fname, &statbuf) < 0)
	{
		if (errno == ENOENT)
			return false;

		ereport(FATAL,
				errcode_for_file_access(),
				errmsg("stat failed for undo file \"%s\": %m", fname));
	}

	return true;
}

/*
 * undolog_file_size()
 *
 * Returns the size of the file corresponding to the specified xid.
 * If the file does not exist, returns 0.
 */
static Size
undolog_file_size(FullTransactionId xid)
{
	char fname[MAXPGPATH];
	struct stat sbuf;
	int fd;

	UndoLogSetFilename(fname, xid);
	fd = BasicOpenFile(fname, PG_BINARY | O_RDONLY);

	if (fd < 0)
	{
		if (errno == ENOENT)
			return 0;

		ereport(FATAL,
				errcode_for_file_access(),
				errmsg("failed to open ulog file \"%s\": %m", fname));
	}

	if (fstat(fd, &sbuf) < 0)
		ereport(FATAL,
				errcode_for_file_access(),
				errmsg("failed to stat ulog file \"%s\": %m", fname));

	close(fd);

	return sbuf.st_size;
}

/*
 * undolog_flush_slot - flush a slot into file
 *
 * Flushes the slot data to the undo log file, then clears the slot.
 * The caller must ensure that the slot is not modified while this function is
 * executing.
 * Creates the file if it does not already exist.
 * If keep is true, the emptied slot remains assigned to the previous xid.
 */
static void
undolog_flush_slot(UndoLogSlot *slot, bool keep)
{
	char fname[MAXPGPATH];
	int fd;
	int ret;

	/* return if no data */
	if (slot->len == 0)
		return;

	UndoLogSetFilename(fname, slot->xid);

	fd = BasicOpenFile(fname, PG_BINARY | O_WRONLY | O_CREAT);
	if (fd < 0)
		ereport(FATAL,
				errcode_for_file_access(),
				errmsg("failed to open or create undo file \"%s\": %m", fname));

	/*
	 * Write the file header if this is the first time the undo log is
	 * being written. The slot buffer doesn't include the header part, so we
	 * write it manually. The header write has already been WAL-logged in
	 * undolog_find_slot().
	 */
	if (slot->off == sizeof(UndoLogFileHeader))
	{
		UndoLogFileHeader fheader;
		Size			  len = sizeof(fheader);

		fheader.magic = ULOG_FILE_MAGIC;
		fheader.crashed = false;
		ret = pg_pwrite(fd, &fheader, len, 0);
		if (ret != len)
			ereport(FATAL,
					errcode_for_file_access(),
					errmsg("failed to write to undo file \"%s\": %m", fname));
	}

	ret = pg_pwrite(fd,	slot->buf, slot->len, slot->off);

	if (ret != slot->len)
		ereport(FATAL,
				errcode_for_file_access(),
				errmsg("failed to write to undo file \"%s\": %m", fname));

	close(fd);

	/* clear the slot buffer */
	slot->off += slot->len;
	slot->len = 0;

	/* release this slot */
	if (!keep)
		slot->xid = InvalidFullTransactionId;

	return;
}

/*
 * undolog_find_slot - returns a slot for the specified top-xid
 *
 * If acquire is true, acquires a slot and creates a new undo log if this is
 * the first call for the xid. Otherwise, returns NULL if no slot is found for
 * the xid.
 *
 * The returned slot is exclusively locked.
 */
static UndoLogSlot *
undolog_find_slot(FullTransactionId xid, bool acquire)
{
	UndoLogSlot *slot;
	int			slot_to_use;

	Assert(FullTransactionIdIsValid(xid));

	/* fast path for currently active slot */
	if (ULogLocal.current_slot > -1)
	{
		slot = &ULogShared->slots[ULogLocal.current_slot];

		LWLockAcquire(&slot->lock, LW_EXCLUSIVE);

		/* return it if the slot has not been stolen by another transaction */
		if (FullTransactionIdEquals(slot->xid, xid))
			return slot;

		LWLockRelease(&slot->lock);

		/*
		 * No other processes are expected to acquire a slot for this xid.
		 * Continue searching for an available slot.
		 */
		ULogLocal.current_slot = -1;
		ULogLocal.current_xid = InvalidFullTransactionId;
	}

	/* no active slot found; return NULL if not set to acquire a new one */
	if (!acquire)
		return NULL;

	/* Search for an invalid slot or the slot with the oldest xid. */
	slot_to_use = -1;
	for (int i = 0 ; i < ULOG_SLOT_NUM ; i++)
	{
		slot = &ULogShared->slots[i];

		LWLockAcquire(&slot->lock, LW_EXCLUSIVE);

		/* slot for this xid should not exist */
		Assert(!FullTransactionIdEquals(slot->xid, xid));

		/* use invalid slot unconditionally */
		if (!FullTransactionIdIsValid(slot->xid))
		{
			/* Replace the slot to use. Release the previous lock if any. */
			if (slot_to_use >= 0)
				LWLockRelease(&ULogShared->slots[slot_to_use].lock);

			slot_to_use = i;
			break;
		}

		/* determine the oldest slot */
		if (slot_to_use < 0)
			slot_to_use = i;
		else if (FullTransactionIdPrecedes(slot->xid,
										   ULogShared->slots[slot_to_use].xid))
		{
			/* Replace the slot to use. Release the previous lock if any. */
			if (slot_to_use >= 0)
				LWLockRelease(&ULogShared->slots[slot_to_use].lock);

			slot_to_use = i;
		}
		else
			LWLockRelease(&slot->lock);
	}

	Assert(slot_to_use >= 0);
	ULogLocal.current_slot = slot_to_use;
	slot = &ULogShared->slots[slot_to_use];

	/* flush the buffered data if any */
	if (FullTransactionIdIsValid(slot->xid))
		undolog_flush_slot(slot, false);

	/*
	 * A partially written file may exist for this xid. In that case, set the
	 * offset based on the file size.
	 */
	ULogLocal.current_xid = slot->xid = xid;
	slot->off = undolog_file_size(xid);
	slot->len = 0;

	if (slot->off == 0)
	{
		/*
		 * This is the first time the undo log is being written. Emit WAL
		 * records for the creation of the file and a write for the header
		 * part. We don’t waste slot space for the header part. It will be
		 * written by undolog_flush_slot().
		 */
		xl_ulog_create crec;
		xl_ulog_write *wrec;
		UndoLogFileHeader *fheader;
		Size		bodylen;
		Size		wreclen;
		XLogRecPtr	recptr;

		crec.xid = xid;
		XLogBeginInsert();
		XLogRegisterData((char *) &crec, sizeof(crec));
		(void) XLogInsert(RM_ULOG_ID, XLOG_ULOG_CREATE);

		bodylen = sizeof(UndoLogFileHeader);
		wreclen = sizeof(xl_ulog_write) + bodylen;
		wrec = undolog_ensure_buffer(wreclen);

		wrec->topxid = xid;
		wrec->subxid = InvalidFullTransactionId;
		wrec->off = 0;
		wrec->len = bodylen;
		fheader = (UndoLogFileHeader *) &wrec->bytes;
		fheader->magic = ULOG_FILE_MAGIC;
		fheader->crashed = false;

		XLogBeginInsert();
		XLogRegisterData((char *) wrec, wreclen);
		recptr = XLogInsert(RM_ULOG_ID, XLOG_ULOG_WRITE);
		XLogFlush(recptr);

		/* adjust file offset */
		slot->off = bodylen;
	}

	return slot;
}

/*
 * undolog_remove_file() - Removes a file specified by full transaction ID.
 *
 * The file must already have been closed.
 */
static void
undolog_remove_file(FullTransactionId xid)
{
	char fname[MAXPGPATH];

	UndoLogSetFilename(fname, xid);

	durable_unlink(fname, FATAL);
}

/*
 * undolog_mark_xid_as_crashed() - Mark an undo log file as "crashed"
 *
 * When executing an undo log file, this attribute will be passed to the "undo"
 * rmgr callback functions.
 */
static void
undolog_mark_xid_as_crashed(FullTransactionId xid)
{
	char fname[MAXPGPATH];
	int fd;
	UndoLogFileHeader fheader;

	/* no slot should not exist */
	Assert(undolog_find_slot(xid, false) == NULL);

	UndoLogSetFilename(fname, xid);
	fd = BasicOpenFile(fname, PG_BINARY | O_RDWR);
	if (fd < 0)
	{
		if (errno == ENOENT)
			return;

		ereport(FATAL,
				errcode_for_file_access(),
				errmsg("failed to open undo file \"%s\": %m", fname));
	}

	if (read(fd, &fheader, sizeof(fheader)) < sizeof(fheader))
		ereport(FATAL,
				errcode_for_file_access(),
				errmsg("failed to read undo log file \"%s\": %m", fname));

	if (fheader.magic != ULOG_FILE_MAGIC)
		ereport(FATAL,
				errcode_for_file_access(),
				errmsg("magic does not match for undo log file \"%s\"", fname));

	fheader.crashed = true;

	if (pg_pwrite(fd, &fheader, sizeof(fheader), 0) != sizeof(fheader))
		ereport(FATAL,
				errcode_for_file_access(),
				errmsg("failed to write to undo file \"%s\": %m", fname));
	close(fd);
}

/*
 * undolog_drop_ulog() - Release slot then remove file if any.
 */
static void
undolog_drop_ulog(FullTransactionId xid)
{
	UndoLogSlot *slot;

	Assert(FullTransactionIdIsValid(xid));

	/*
	 * If the shortcut is dangling, it means our slot has been stolen, and no
	 * slot is currently associated with our XID. In this case, the contents
	 * have already been written to the corresponding file.
 	 */
	if (ULogLocal.current_slot >= 0)
	{
		slot = &ULogShared->slots[ULogLocal.current_slot];

		LWLockAcquire(&slot->lock, LW_EXCLUSIVE);

		if (FullTransactionIdEquals(slot->xid, xid))
			slot->xid = InvalidFullTransactionId;

		LWLockRelease(&slot->lock);

		ULogLocal.current_slot = -1;
		ULogLocal.current_xid = InvalidFullTransactionId;
	}

	Assert(!FullTransactionIdIsValid(ULogLocal.current_xid));

	/* remove the file if any */
	if (undolog_file_exists(xid))
		undolog_remove_file(xid);
}


/*
 * UndoLogWrite() - Writes an undolog record
 *
 * This write is WAL-logged.
 */
void
UndoLogWrite(RmgrId rmgr, uint8 info, void *data, int len)
{
	FullTransactionId topxid;
	FullTransactionId subxid;
	int reclen = sizeof(UndoLogRecord) + len;
	int wreclen = sizeof(xl_ulog_write) + reclen;
	xl_ulog_write  *wrec;
	UndoLogRecord  *rec;
	XLogRecPtr		recptr;
	UndoLogSlot	   *slot;

	Assert(!RecoveryInProgress());
	Assert(!IsParallelWorker());

	if (!IsUnderPostmaster)
		return;

	/*
	 * The following lines may assign new transaction IDs. While this is
	 * somewhat clumsy, the caller needs to assign them soon.
	 */
	topxid = GetTopFullTransactionId();
	subxid = GetCurrentFullTransactionId();

	/* the caller may set rmgr bits only */
	Assert((info & ~ULR_RMGR_INFO_MASK) == 0);

	/*
	 * Since this call uses the common buffer returned by
	 * undolog_ensure_buffer(), which is also used immediately below, it must
	 * be placed before the buffer is used there.
	 */
	slot =  undolog_find_slot(topxid, true);

	/* build undo record as a part of WAL record to avoid copying */
	wrec = undolog_ensure_buffer(wreclen);
	rec = (UndoLogRecord *) &wrec->bytes;
	rec->ul_tot_len = reclen;
	rec->ul_rmid = rmgr;
	rec->ul_info = info;
	rec->ul_xid = subxid;

	if (len > 0)
		memcpy((char *)rec + sizeof(UndoLogRecord), data, len);

	/*
	 * Write an XLOG record for this undo log record. It is crucial to flush
	 * immediately, as this record is needed to cancel the action taken right
	 * after if this transaction crashes before the commit.
	 */
	wrec->topxid = topxid;
	wrec->subxid = subxid;
	wrec->off = slot->off + slot->len;
	wrec->len = reclen;
	XLogBeginInsert();
	XLogRegisterData((char *) wrec, wreclen);
	recptr = XLogInsert(RM_ULOG_ID, XLOG_ULOG_WRITE);
	XLogFlush(recptr);

	/* flush if the slot is about to overflow */
	if (slot->len + reclen > ULOG_SLOT_BUF_LEN)
		undolog_flush_slot(slot, true);

	/* append the record to the slot */
	Assert (slot->len + reclen <= ULOG_SLOT_BUF_LEN);
	memcpy(slot->buf + slot->len, rec, reclen);
	slot->len += reclen;

	LWLockRelease(&slot->lock);
}

/* Helper routine for calling event callbacks */
static void
undolog_event_call(ULogEvent event)
{
	for (int rmid = 0; rmid <= RM_MAX_ID; rmid++)
	{
		if (RmgrUndo[rmid].rm_name == NULL)
			continue;

		if (RmgrUndo[rmid].rm_undo_event != NULL)
			RmgrUndo[rmid].rm_undo_event(event);
	}
}

/*
 * ulog_process_ulog() - Processes an undo log file.
 *
 * 'cxt' specifies the operation context. redo is true during recovery.
 *
 * XXX: While redo is determined solely by cxt, the two parameters are
 * currently provided separately.
 */
static void
undolog_process_ulog(char *fname, ULogContext cxt, bool redo)
{
	int fd;
	struct stat sb;
	char *buf;
	char *p;
	char *endptr;
	UndoLogFileHeader *phead;

	fd = BasicOpenFile(fname, PG_BINARY | O_RDONLY);
	if (stat(fname, &sb) < 0)
		ereport(FATAL,
				errcode_for_file_access(),
				errmsg("could not stat undo log file \"%s\": %m", fname));
	buf = palloc(sb.st_size);
	if (read(fd, buf, sb.st_size) < sb.st_size)
		ereport(FATAL,
				errcode_for_file_access(),
				errmsg("failed to read undo log file \"%s\": %m", fname));
	close(fd);

	p = buf;
	endptr = buf + sb.st_size;
	phead = (UndoLogFileHeader *) p;
	p += sizeof(*phead);
	if (phead->magic != ULOG_FILE_MAGIC)
		ereport(FATAL,
				errcode_for_file_access(),
				errmsg("magic does not match for undo log file \"%s\"", fname));

	while (p < endptr)
	{
		UndoLogRecord *rec = (UndoLogRecord *)p;

		RmgrUndo[rec->ul_rmid].rm_undo(rec, cxt, redo, phead->crashed);

		p += rec->ul_tot_len;
	}
	pfree(buf);
	
	/* invoke end-of-xact callbacks */
	undolog_event_call(ULOGEVENT_XACTEND);
}

/*
 * UndoLog_UndoByXid()
 *
 * Processes undo logs for the specified transactions, used when finishing
 * prepared transactions or during commits and aborts in recovery.
 *
 * children is the list of subtransaction IDs of the xid, with a length of
 * nchildren.
 */
void
UndoLog_UndoByXid(bool isCommit, TransactionId xid,
						int nchildren, TransactionId *children)
{
	uint32				nextepoch;
	TransactionId		nextxid;
	uint32 				epoch;
	FullTransactionId	fxid;
	UndoLogSlot *slot;

	nextepoch = EpochFromFullTransactionId(TransamVariables->nextXid);
	nextxid = XidFromFullTransactionId(TransamVariables->nextXid);

	/* Adjust epoch, if needed. */
	if (xid <= nextxid)
		epoch = nextepoch;
	else
		epoch = nextepoch - 1;

	/* process undo logs */
	fxid = FullTransactionIdFromEpochAndXid(epoch, xid);

	slot = undolog_find_slot(fxid, false);
	if (slot)
	{
		undolog_flush_slot(slot, false);
		LWLockRelease(&slot->lock);
	}

	if (undolog_file_exists(fxid))
	{
		char fname[MAXPGPATH];
		ULogContext cxt;

		if (isCommit)
			cxt = ULOGCXT_COMMIT;
		else
			cxt = ULOGCXT_ABORT;

		UndoLogSetFilename(fname, fxid);
		undolog_process_ulog(fname, cxt, false);
		undolog_drop_ulog(fxid);
	}
}

/*
 * AtEOXact_UndoLog() - At end-of-xact processing of undo logs.
 *
 * Cleans up any undo logs emitted by the transaction, if present.
 * During normal operation, the caller should pass InvalidTransactionId as xid.
 * During recovery, it should pass the target transaction ID.
 */
void
AtEOXact_UndoLog(TransactionId xid)
{
	FullTransactionId fxid = ULogLocal.current_xid;

	if (TransactionIdIsValid(xid))
	{
		FullTransactionId next_fxid;
		TransactionId oldest_xid;
		TransactionId next_xid;
		uint32		oldest_epoch;
		
		LWLockAcquire(XactTruncationLock, LW_SHARED);
		next_fxid = TransamVariables->nextXid;
		oldest_xid = TransamVariables->oldestClogXid;
		LWLockRelease(XactTruncationLock);

		/* Generate full xid for oldest_xid based on next_fxid */
		next_xid = XidFromFullTransactionId(next_fxid);
		oldest_epoch = EpochFromFullTransactionId(next_fxid);

		/* adjust epoch for oldest xid */
		if (oldest_xid > next_xid)
			oldest_epoch--;

		fxid = FullTransactionIdFromEpochAndXid(oldest_epoch, xid);
	}

	if (FullTransactionIdIsValid(fxid))
		undolog_drop_ulog(fxid);
}

/*
 * AtPrepare_UndoLog()
 *
 * Flush undo log slot if used then blow away the xid list for the current
 * transaction. The file will be removed at commit of the prepared transaction.
 */
void
AtPrepare_UndoLog(void)
{
	FullTransactionId xid = GetTopFullTransactionId();
	UndoLogSlot *slot = undolog_find_slot(xid, false);

	if (slot)
	{
		undolog_flush_slot(slot, false);
		LWLockRelease(&slot->lock);
	}
}

static void
undolog_cleanup_init(void)
{
	undolog_event_call(ULOGEVENT_CLEANUP_INIT);
}

void
UndoLogRecoveryEnd(void)
{
	undolog_event_call(ULOGEVENT_RECOVERY_END);
}

/*
 * UndoLogCleanup() - On-recovery cleanup of undo log
 *
 * This function is called after ULOG file consistency is established, either
 * when recovery reaches consistency or after recovery finishes if hot standby
 * is not active.
 */
void
UndoLogCleanup(bool end_of_recovery)
{
	DIR		   *dirdesc;
	struct dirent *de;
	char fname[MAXPGPATH];
	char *p;

	undolog_cleanup_init();

	/*
	 * flush all in-memory undo logs, no need for locking sinced we're the only
	 * process working on this array.
	 */
	for (int i = 0 ; i < ULOG_SLOT_NUM ; i++)
	{
		UndoLogSlot *slot = &ULogShared->slots[i];

		if (FullTransactionIdIsValid(slot->xid))
			undolog_flush_slot(slot, false);

		slot->xid = InvalidFullTransactionId;
	}

	snprintf(fname, MAXPGPATH, "%s/", UNDOLOG_DIR);
	p = fname + strlen(fname);

	/* scan through all undo log files */
	dirdesc = AllocateDir(UNDOLOG_DIR);
	while ((de = ReadDir(dirdesc, UNDOLOG_DIR)) != NULL)
	{
		FullTransactionId	log_fxid;
		TransactionId		log_xid;
		FullTransactionId	next_fxid;
		FullTransactionId	oldest_fxid;
		TransactionId		oldest_xid;
		TransactionId		next_xid;
		uint32				oldest_epoch;
		ULogContext			cxt;
		bool				xact_prepared;

		if (strlen(de->d_name) != 16 ||
			strspn(de->d_name, "01234567890abcdef") < strlen(de->d_name))
			continue;

		strncpy(p, de->d_name, 32);

		/*
		 *  Make sure the log's xid is valid.
		 */
		log_fxid = FullTransactionIdFromU64(strtou64(de->d_name, NULL, 16));
		log_xid = XidFromFullTransactionId(log_fxid);

		LWLockAcquire(XactTruncationLock, LW_SHARED);
		next_fxid = TransamVariables->nextXid;
		oldest_xid = TransamVariables->oldestClogXid;
		LWLockRelease(XactTruncationLock);

		/* Generate full xid for oldest_xid based on next_fxid */
		next_xid = XidFromFullTransactionId(next_fxid);
		oldest_epoch = EpochFromFullTransactionId(next_fxid);

		/* adjust epoch for oldest xid */
		if (oldest_xid > next_xid)
			oldest_epoch--;

		oldest_fxid =
			FullTransactionIdFromEpochAndXid(oldest_epoch, oldest_xid);

		/* check the ulog xid */
		if (FullTransactionIdPrecedes(log_fxid, oldest_fxid))
			ereport(FATAL,
					errcode_for_file_access(),
					errmsg("undolog found for too-old transaction %llu",
						   (long long unsigned int) U64FromFullTransactionId(log_fxid)));

		/* All transactions with undo log must be in-progress. */
		if (!TransactionIdIsInProgress(log_xid))
			ereport(FATAL,
					errcode_for_file_access(),
					errmsg("undolog found for non-acitve transaction: %llu",
						   (long long unsigned int) U64FromFullTransactionId(log_fxid)));


		/*
		 * Let undo routines perform cleanup tasks with appropriate
		 * assumptions.  If the transaction is prepared or when recovery is
		 * reaching consistency, assume it is active; otherwise, perform abort
		 * cleanups.
		 */
		xact_prepared = TwoPhaseXidExists(log_xid, false);

		if (!end_of_recovery || xact_prepared)
			cxt = ULOGCXT_PREPARED;
		else
			cxt = ULOGCXT_CLEANUP;

		undolog_process_ulog(fname, cxt, true);

		if (undolog_file_exists(log_fxid))
		{
			if (!xact_prepared)
				undolog_remove_file(log_fxid);
			else
				undolog_mark_xid_as_crashed(log_fxid);
		}
	}
	FreeDir(dirdesc);
}

/*
 * undollg_redo()
 *
 * Recovery routine for undo logs.
 */
void
undolog_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_ULOG_CREATE)
	{
		xl_ulog_create	   *rec = (xl_ulog_create *) XLogRecGetData(record);
		char fname[MAXPGPATH];
		int fd;

		/*
		 * We don't check for the existence of the log. Although the log should
		 * not be found in a consistent state, it may appear during the
		 * inconsistent period in recovery.
		 */
		UndoLogSetFilename(fname, rec->xid);
		fd = BasicOpenFile(fname, PG_BINARY | O_WRONLY | O_CREAT);
		if (fd < 0)
			ereport(FATAL,
					errcode_for_file_access(),
					errmsg("failed to open or create undo file \"%s\": %m", fname));
		close(fd);
	}
	else if (info == XLOG_ULOG_WRITE)
	{
		char fname[MAXPGPATH];
		int fd;
		xl_ulog_write  *rec = (xl_ulog_write *) XLogRecGetData(record);
		ssize_t			ret;

		UndoLogSetFilename(fname, rec->topxid);
		fd = BasicOpenFile(fname, PG_BINARY | O_WRONLY);
		if (fd < 0)
			ereport(FATAL,
					errcode_for_file_access(),
					errmsg("failed to open or create undo file \"%s\": %m", fname));
		ret = pg_pwrite(fd, rec->bytes, rec->len, rec->off);
		if (ret != rec->len)
		ereport(FATAL,
				errcode_for_file_access(),
				errmsg("failed to write to undo file \"%s\": %m", fname));

		close(fd);
	}
}

/*
 * CheckPointUndoLog
 *
 * This is called during a checkpoint.  It must ensure that any undo log writes
 * that were WAL-logged before the start of the checkpoint are securely flushed
 * to disk so that we won't lose their existence and content before this
 * checkpoint start.
 */
void
CheckPointUndoLog(void)
{
	bool		written = false;

	if (!IsUnderPostmaster)
		return;

	for (int i = 0 ; i < ULOG_SLOT_NUM ; i++)
	{
		UndoLogSlot *slot = &ULogShared->slots[i];

		LWLockAcquire(&slot->lock, LW_EXCLUSIVE);

		if (FullTransactionIdIsValid(slot->xid))
		{
			undolog_flush_slot(slot, true);
			written = true;
		}

		slot->xid = InvalidFullTransactionId;

		LWLockRelease(&slot->lock);
	}


	/* Sync the directory if any files have been written to it. */
	if (written)
		fsync_fname(UNDOLOG_DIR, true);
}
