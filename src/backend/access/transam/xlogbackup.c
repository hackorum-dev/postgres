/*-------------------------------------------------------------------------
 *
 * xlogbackup.c
 *		Internal routines for base backups.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *      src/backend/access/transam/xlogbackup.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogbackup.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

/* Control array for in-progress backups */
BackupCtlData *BackupCtl = NULL;

static void BackupCtlShmemRequest(void *arg);
static void BackupCtlShmemInit(void *arg);

const ShmemCallbacks BackupCtlShmemCallbacks = {
	.request_fn = BackupCtlShmemRequest,
	.init_fn = BackupCtlShmemInit,
};

/* This backend's backup control structure in the shared memory array */
BackupInProgress *MyBackupInProgress = NULL;

/* GUC */
int max_concurrent_backups = 10; /* the maximum number of concurrent backups */

/*
 * Register shared memory space for the backup control structure
 */
static void BackupCtlShmemRequest(void *arg)
{
	Size size;

	/* max_concurrent_backups is at least 1 */
	Assert(max_concurrent_backups > 0);

	size = offsetof(BackupCtlData, backups);
	size = add_size(size, mul_size(max_concurrent_backups, sizeof(BackupInProgress)));
	ShmemRequestStruct(.name = "Backup Ctl",
		.size = size,
		.ptr = (void **)&BackupCtl);
}

/*
 * Initialize shared memory for the backup control structure.
 *
 * No cleanup is needed on shmem_exit.
 */
static void BackupCtlShmemInit(void *arg)
{
	int i;

	for (i = 0; i < max_concurrent_backups; i++)
	{
		BackupCtl->backups[i].startpoint = InvalidXLogRecPtr;
	}
	BackupCtl->oldestStartpoint = InvalidXLogRecPtr;
}

/*
 * Register this backup's startpoint.
 *
 * We update the oldest startpoint across all in-progress backups here.
 *
 */

void
RegisterBackupStartpoint(XLogRecPtr startpoint)
{
	int i;

	Assert(BackupCtl != NULL);
	Assert(MyBackupInProgress == NULL);

	LWLockAcquire(BackupControlLock, LW_EXCLUSIVE);
	for (i = 0; i < max_concurrent_backups; i++)
	{
		/* Find the first unused entry */
		if (BackupCtl->backups[i].startpoint == InvalidXLogRecPtr)
		{
			BackupCtl->backups[i].startpoint = startpoint;
			MyBackupInProgress = &BackupCtl->backups[i];
			/* Update the oldest startpoint if necessary */
			if (!XLogRecPtrIsValid(BackupCtl->oldestStartpoint) ||
				startpoint < BackupCtl->oldestStartpoint)
					BackupCtl->oldestStartpoint = startpoint;
			break;
		}
	}
	LWLockRelease(BackupControlLock);

	/* If the array is full, bail out */
	if (i == max_concurrent_backups)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("maximum number of concurrent backups reached"),
				 errhint("Wait for another backup to finish, or increase max_concurrent_backups.")));
}

/*
 * Unregister this backup.
 *
 * We also recalculate the oldest startpoint across all remaining
 * in-progress backups.
 *
 */

void
UnregisterBackupStartpoint(void)
{
	XLogRecPtr candidate_startpoint;

	if (MyBackupInProgress == NULL)
		return;

	Assert(BackupCtl != NULL);

	LWLockAcquire(BackupControlLock, LW_EXCLUSIVE);

	MyBackupInProgress->startpoint = InvalidXLogRecPtr;

	/* Invalidate the oldest startpoint */
	BackupCtl->oldestStartpoint = InvalidXLogRecPtr;
	/* Scan the array to find the new oldest startpoint */
	for (int i = 0; i < max_concurrent_backups; i++)
	{
		candidate_startpoint = BackupCtl->backups[i].startpoint;
		if (XLogRecPtrIsValid(candidate_startpoint) &&
			(!XLogRecPtrIsValid(BackupCtl->oldestStartpoint) ||
			 candidate_startpoint < BackupCtl->oldestStartpoint))
			BackupCtl->oldestStartpoint  = candidate_startpoint;
	}

	LWLockRelease(BackupControlLock);

	MyBackupInProgress = NULL;
}

/*
 * Return the precomputed minimum startpoint across all in-progress backups
 * to use when determining what WAL segments to keep.
 */

XLogRecPtr
GetOldestBackupStartLSN(void)
{
	XLogRecPtr	retval;

	LWLockAcquire(BackupControlLock, LW_SHARED);
	retval = BackupCtl->oldestStartpoint;
	LWLockRelease(BackupControlLock);

	return retval;
}

/*
 * Build contents for backup_label or backup history file.
 *
 * When ishistoryfile is true, it creates the contents for a backup history
 * file, otherwise it creates contents for a backup_label file.
 *
 * Returns the result generated as a palloc'd string.
 */
char *
build_backup_content(BackupState *state, bool ishistoryfile)
{
	char		startstrbuf[128];
	char		startxlogfile[MAXFNAMELEN]; /* backup start WAL file */
	XLogSegNo	startsegno;
	StringInfoData result;

	Assert(state != NULL);

	initStringInfo(&result);

	/* Use the log timezone here, not the session timezone */
	pg_strftime(startstrbuf, sizeof(startstrbuf), "%Y-%m-%d %H:%M:%S %Z",
				pg_localtime(&state->starttime, log_timezone));

	XLByteToSeg(state->startpoint, startsegno, wal_segment_size);
	XLogFileName(startxlogfile, state->starttli, startsegno, wal_segment_size);
	appendStringInfo(&result, "START WAL LOCATION: %X/%08X (file %s)\n",
					 LSN_FORMAT_ARGS(state->startpoint), startxlogfile);

	if (ishistoryfile)
	{
		char		stopxlogfile[MAXFNAMELEN];	/* backup stop WAL file */
		XLogSegNo	stopsegno;

		XLByteToSeg(state->stoppoint, stopsegno, wal_segment_size);
		XLogFileName(stopxlogfile, state->stoptli, stopsegno, wal_segment_size);
		appendStringInfo(&result, "STOP WAL LOCATION: %X/%08X (file %s)\n",
						 LSN_FORMAT_ARGS(state->stoppoint), stopxlogfile);
	}

	appendStringInfo(&result, "CHECKPOINT LOCATION: %X/%08X\n",
					 LSN_FORMAT_ARGS(state->checkpointloc));
	appendStringInfoString(&result, "BACKUP METHOD: streamed\n");
	appendStringInfo(&result, "BACKUP FROM: %s\n",
					 state->started_in_recovery ? "standby" : "primary");
	appendStringInfo(&result, "START TIME: %s\n", startstrbuf);
	appendStringInfo(&result, "LABEL: %s\n", state->name);
	appendStringInfo(&result, "START TIMELINE: %u\n", state->starttli);

	if (ishistoryfile)
	{
		char		stopstrfbuf[128];

		/* Use the log timezone here, not the session timezone */
		pg_strftime(stopstrfbuf, sizeof(stopstrfbuf), "%Y-%m-%d %H:%M:%S %Z",
					pg_localtime(&state->stoptime, log_timezone));

		appendStringInfo(&result, "STOP TIME: %s\n", stopstrfbuf);
		appendStringInfo(&result, "STOP TIMELINE: %u\n", state->stoptli);
	}

	/* either both istartpoint and istarttli should be set, or neither */
	Assert(XLogRecPtrIsValid(state->istartpoint) == (state->istarttli != 0));
	if (XLogRecPtrIsValid(state->istartpoint))
	{
		appendStringInfo(&result, "INCREMENTAL FROM LSN: %X/%08X\n",
						 LSN_FORMAT_ARGS(state->istartpoint));
		appendStringInfo(&result, "INCREMENTAL FROM TLI: %u\n",
						 state->istarttli);
	}

	return result.data;
}
