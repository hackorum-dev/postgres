/*
 * xlogrecovery.h
 *
 * Functions for WAL recovery and standby mode
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogrecovery.h
 */
#ifndef XLOGRECOVERY_H
#define XLOGRECOVERY_H

#include "access/xlogreader.h"
#include "catalog/pg_control.h"
#include "lib/stringinfo.h"
#include "utils/timestamp.h"

/*
 * The flag indicates if we allow hot standby queries to be run.
 */
#define SX_HOT_STANDBY_ACTIVE			0x01	/* SX: Startup Xlog */
/*
 * The flag indicates if a standby promotion has been triggered.
 */
#define SX_PROMOTE_IS_TRIGGERED			0x02
/*
 * When SX_ARCHIVE_RECOVERY_REQUESTED is set, archive recovery was requested,
 * i.e. signal files were present.  When SX_IN_ARCHIVE_RECOVERY is set, we are
 * currently recovering using offline XLOG archives.  These variables are only
 * valid in the startup process.
 *
 * When SX_ARCHIVE_RECOVERY_REQUESTED is set, but SX_IN_ARCHIVE_RECOVERY is
 * not, we're currently performing crash recovery using only XLOG files in
 * pg_wal, but will switch to using offline XLOG archives as soon as we reach
 * the end of WAL in pg_wal.
 */
#define SX_ARCHIVE_RECOVERY_REQUESTED	0x04
#define SX_IN_ARCHIVE_RECOVERY			0x08
/*
 * When SX_STANDBY_MODE_REQUESTED is set, standby mode was requested, i.e.
 * standby.signal file was present.  When SX_IN_STANDBY_MODE is set, we are
 * currently in standby mode.  These variables are only valid in the startup
 * process. They work similarly to SX_ARCHIVE_RECOVERY_REQUESTED and
 * SX_IN_ARCHIVE_RECOVERY.
 */
#define SX_STANDBY_MODE_REQUESTED		0x10
#define SX_IN_STANDBY_MODE				0x20

/*
 * Recovery target type.
 * Only set during a Point in Time recovery, not when in standby mode.
 */
typedef enum
{
	RECOVERY_TARGET_UNSET,
	RECOVERY_TARGET_XID,
	RECOVERY_TARGET_TIME,
	RECOVERY_TARGET_NAME,
	RECOVERY_TARGET_LSN,
	RECOVERY_TARGET_IMMEDIATE,
} RecoveryTargetType;

/*
 * Recovery target TimeLine goal
 */
typedef enum
{
	RECOVERY_TARGET_TIMELINE_CONTROLFILE,
	RECOVERY_TARGET_TIMELINE_LATEST,
	RECOVERY_TARGET_TIMELINE_NUMERIC,
} RecoveryTargetTimeLineGoal;

/* Recovery pause states */
typedef enum RecoveryPauseState
{
	RECOVERY_NOT_PAUSED,		/* pause not requested */
	RECOVERY_PAUSE_REQUESTED,	/* pause requested, but not yet paused */
	RECOVERY_PAUSED,			/* recovery is paused */
} RecoveryPauseState;

/* User-settable GUC parameters */
extern PGDLLIMPORT bool recoveryTargetInclusive;
extern PGDLLIMPORT int recoveryTargetAction;
extern PGDLLIMPORT int recovery_min_apply_delay;
extern PGDLLIMPORT char *PrimaryConnInfo;
extern PGDLLIMPORT char *PrimarySlotName;
extern PGDLLIMPORT char *recoveryRestoreCommand;
extern PGDLLIMPORT char *recoveryEndCommand;
extern PGDLLIMPORT char *archiveCleanupCommand;

/* indirectly set via GUC system */
extern PGDLLIMPORT TransactionId recoveryTargetXid;
extern PGDLLIMPORT char *recovery_target_time_string;
extern PGDLLIMPORT TimestampTz recoveryTargetTime;
extern PGDLLIMPORT const char *recoveryTargetName;
extern PGDLLIMPORT XLogRecPtr recoveryTargetLSN;
extern PGDLLIMPORT RecoveryTargetType recoveryTarget;
extern PGDLLIMPORT bool wal_receiver_create_temp_slot;
extern PGDLLIMPORT RecoveryTargetTimeLineGoal recoveryTargetTimeLineGoal;
extern PGDLLIMPORT TimeLineID recoveryTargetTLIRequested;
extern PGDLLIMPORT TimeLineID recoveryTargetTLI;

/* Have we already reached a consistent database state? */
extern PGDLLIMPORT bool reachedConsistency;

extern Size XLogRecoveryShmemSize(void);
extern void XLogRecoveryShmemInit(void);

extern void InitWalRecovery(ControlFileData *ControlFile,
							bool *wasShutdown_ptr, bool *haveBackupLabel_ptr,
							bool *haveTblspcMap_ptr);
extern void PerformWalRecovery(void);

/*
 * FinishWalRecovery() returns this.  It contains information about the point
 * where recovery ended, and why it ended.
 */
typedef struct
{
	/*
	 * Information about the last valid or applied record, after which new WAL
	 * can be appended.  'lastRec' is the position where the last record
	 * starts, and 'endOfLog' is its end.  'lastPage' is a copy of the last
	 * partial page that contains endOfLog (or NULL if endOfLog is exactly at
	 * page boundary).  'lastPageBeginPtr' is the position where the last page
	 * begins.
	 *
	 * endOfLogTLI is the TLI in the filename of the XLOG segment containing
	 * the last applied record.  It could be different from lastRecTLI, if
	 * there was a timeline switch in that segment, and we were reading the
	 * old WAL from a segment belonging to a higher timeline.
	 */
	XLogRecPtr	lastRec;		/* start of last valid or applied record */
	TimeLineID	lastRecTLI;
	XLogRecPtr	endOfLog;		/* end of last valid or applied record */
	TimeLineID	endOfLogTLI;

	XLogRecPtr	lastPageBeginPtr;	/* LSN of page that contains endOfLog */
	char	   *lastPage;		/* copy of the last page, up to endOfLog */

	/*
	 * abortedRecPtr is the start pointer of a broken record at end of WAL
	 * when recovery completes; missingContrecPtr is the location of the first
	 * contrecord that went missing.  See CreateOverwriteContrecordRecord for
	 * details.
	 */
	XLogRecPtr	abortedRecPtr;
	XLogRecPtr	missingContrecPtr;

	/* short human-readable string describing why recovery ended */
	char	   *recoveryStopReason;

	/*
	 * If standby or recovery signal file was found, these flags are set
	 * accordingly.
	 */
	bool		standby_signal_file_found;
	bool		recovery_signal_file_found;
} EndOfWalRecoveryInfo;

extern EndOfWalRecoveryInfo *FinishWalRecovery(void);
extern void ShutdownWalRecovery(void);
extern void RemovePromoteSignalFiles(void);

extern bool HotStandbyActive(void);
extern XLogRecPtr GetXLogReplayRecPtr(TimeLineID *replayTLI);
extern RecoveryPauseState GetRecoveryPauseState(void);
extern void SetRecoveryPause(bool recoveryPause);
extern void GetXLogReceiptTime(TimestampTz *rtime, bool *fromStream);
extern TimestampTz GetLatestXTime(void);
extern TimestampTz GetCurrentChunkReplayStartTime(void);
extern XLogRecPtr GetCurrentReplayRecPtr(TimeLineID *replayEndTLI);

extern bits32 GetXLogRecoveryFlags(void);
extern bool PromoteIsTriggered(void);
extern bool CheckPromoteSignal(void);
extern void WakeupRecovery(void);

extern void StartupRequestWalReceiverRestart(void);
extern void XLogRequestWalReceiverReply(void);

extern void RecoveryRequiresIntParameter(const char *param_name, int currValue, int minValue);

extern void xlog_outdesc(StringInfo buf, XLogReaderState *record);

#endif							/* XLOGRECOVERY_H */
