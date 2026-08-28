/*-------------------------------------------------------------------------
 *
 * xlogbackup.h
 *		Definitions for internals of base backups.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/xlogbackup.h
 *-------------------------------------------------------------------------
 */

#ifndef XLOG_BACKUP_H
#define XLOG_BACKUP_H

#include "access/xlogdefs.h"
#include "pgtime.h"

/* Structure to hold backup state. */
typedef struct BackupState
{
	/* Fields saved at backup start */
	/* Backup label name one extra byte for null-termination */
	char		name[MAXPGPATH + 1];
	XLogRecPtr	startpoint;		/* backup start WAL location */
	TimeLineID	starttli;		/* backup start TLI */
	XLogRecPtr	checkpointloc;	/* last checkpoint location */
	pg_time_t	starttime;		/* backup start time */
	bool		started_in_recovery;	/* backup started in recovery? */
	XLogRecPtr	istartpoint;	/* incremental based on backup at this LSN */
	TimeLineID	istarttli;		/* incremental based on backup on this TLI */

	/* Fields saved at the end of backup */
	XLogRecPtr	stoppoint;		/* backup stop WAL location */
	TimeLineID	stoptli;		/* backup stop TLI */
	pg_time_t	stoptime;		/* backup stop time */
} BackupState;

/*
 * Shared memory state of a backup in progress. Here we keep track of its
 * start LSN to ensure checkpoints don't recycle or remove the corresponding
 * WAL segments until we're done.
 *
 * Using a struct instead of a bare XLogRecPtr to allow future extensions.
 */
typedef struct BackupInProgress {
	/*
	 * Each backup triggers its own checkpoint, so their startpoints are
	 * guaranteed to be unique, and we can use InvalidXLogRecPtr to indicate
	 * an available entry
	 */
	XLogRecPtr	startpoint;
} BackupInProgress;

/* Shared memory structure for all in-progress backups
 *
 * It is protected by the LWLock BackupControlLock; exclusive
 * for writers, shared for readers.
 */
typedef struct BackupCtlData {
	/* Minimum startpoint of all in-progress backups */
	XLogRecPtr			oldestStartpoint;
	BackupInProgress 	backups[FLEXIBLE_ARRAY_MEMBER];
} BackupCtlData;

/*
 * Pointer to shared memory
 */
extern PGDLLIMPORT BackupCtlData *BackupCtl;
extern PGDLLIMPORT BackupInProgress *MyBackupInProgress;

/* GUCs */
extern PGDLLIMPORT int max_concurrent_backups;

extern void RegisterBackupStartpoint(XLogRecPtr startpoint);
extern void UnregisterBackupStartpoint(void);
extern XLogRecPtr GetOldestBackupStartLSN(void);

extern char *build_backup_content(BackupState *state,
								  bool ishistoryfile);

#endif							/* XLOG_BACKUP_H */
