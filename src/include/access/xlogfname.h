/*
 * xlogfname.h
 *
 * PostgreSQL write-ahead log file naming macros and definitions.
 *
 * NOTE: this file is intended to contain declarations useful for
 * manipulating the names of XLOG files but not their internal contents.
 *
 * Note: This file must be includable in both frontend and backend contexts,
 * to allow stand-alone tools to deal with WAL files.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogfname.h
 */
#ifndef XLOG_FNAME_H
#define XLOG_FNAME_H

#include "access/xlogseg.h"

/*
 * The XLog directory and control file (relative to $PGDATA)
 */
#define XLOGDIR				"pg_wal"
#define XLOG_CONTROL_FILE	"global/pg_control"

/*
 * These macros encapsulate knowledge about the exact layout of XLog file
 * names, timeline history file names, and archive-status file names.
 */
#define MAXFNAMELEN		64

/* Length of XLog file name */
#define XLOG_FNAME_LEN	   24

/*
 * Generate a WAL segment file name.  Do not use this macro in a helper
 * function allocating the result generated.
 */
#define XLogFileName(fname, tli, logSegNo, wal_segsz_bytes)	\
	snprintf(fname, MAXFNAMELEN, "%08X%08X%08X", tli,		\
			 (uint32) ((logSegNo) / XLogSegmentsPerXLogId(wal_segsz_bytes)), \
			 (uint32) ((logSegNo) % XLogSegmentsPerXLogId(wal_segsz_bytes)))

#define XLogFileNameById(fname, tli, log, seg)	\
	snprintf(fname, MAXFNAMELEN, "%08X%08X%08X", tli, log, seg)

#define IsXLogFileName(fname) \
	(strlen(fname) == XLOG_FNAME_LEN && \
	 strspn(fname, "0123456789ABCDEF") == XLOG_FNAME_LEN)

/*
 * XLOG segment with .partial suffix.  Used by pg_receivewal and at end of
 * archive recovery, when we want to archive a WAL segment but it might not
 * be complete yet.
 */
#define IsPartialXLogFileName(fname)	\
	(strlen(fname) == XLOG_FNAME_LEN + strlen(".partial") &&	\
	 strspn(fname, "0123456789ABCDEF") == XLOG_FNAME_LEN &&		\
	 strcmp((fname) + XLOG_FNAME_LEN, ".partial") == 0)

#define XLogFromFileName(fname, tli, logSegNo, wal_segsz_bytes)	\
	do {												\
		uint32 log;										\
		uint32 seg;										\
		sscanf(fname, "%08X%08X%08X", tli, &log, &seg); \
		*logSegNo = (uint64) log * XLogSegmentsPerXLogId(wal_segsz_bytes) + seg; \
	} while (0)

#define XLogFilePath(path, tli, logSegNo, wal_segsz_bytes)	\
	snprintf(path, MAXPGPATH, XLOGDIR "/%08X%08X%08X", tli,	\
			 (uint32) ((logSegNo) / XLogSegmentsPerXLogId(wal_segsz_bytes)), \
			 (uint32) ((logSegNo) % XLogSegmentsPerXLogId(wal_segsz_bytes)))

#define TLHistoryFileName(fname, tli)	\
	snprintf(fname, MAXFNAMELEN, "%08X.history", tli)

#define IsTLHistoryFileName(fname)	\
	(strlen(fname) == 8 + strlen(".history") &&		\
	 strspn(fname, "0123456789ABCDEF") == 8 &&		\
	 strcmp((fname) + 8, ".history") == 0)

#define TLHistoryFilePath(path, tli)	\
	snprintf(path, MAXPGPATH, XLOGDIR "/%08X.history", tli)

#define StatusFilePath(path, xlog, suffix)	\
	snprintf(path, MAXPGPATH, XLOGDIR "/archive_status/%s%s", xlog, suffix)

#define BackupHistoryFileName(fname, tli, logSegNo, startpoint, wal_segsz_bytes) \
	snprintf(fname, MAXFNAMELEN, "%08X%08X%08X.%08X.backup", tli, \
			 (uint32) ((logSegNo) / XLogSegmentsPerXLogId(wal_segsz_bytes)), \
			 (uint32) ((logSegNo) % XLogSegmentsPerXLogId(wal_segsz_bytes)), \
			 (uint32) (XLogSegmentOffset(startpoint, wal_segsz_bytes)))

#define IsBackupHistoryFileName(fname) \
	(strlen(fname) > XLOG_FNAME_LEN && \
	 strspn(fname, "0123456789ABCDEF") == XLOG_FNAME_LEN && \
	 strcmp((fname) + strlen(fname) - strlen(".backup"), ".backup") == 0)

#define BackupHistoryFilePath(path, tli, logSegNo, startpoint, wal_segsz_bytes)	\
	snprintf(path, MAXPGPATH, XLOGDIR "/%08X%08X%08X.%08X.backup", tli, \
			 (uint32) ((logSegNo) / XLogSegmentsPerXLogId(wal_segsz_bytes)), \
			 (uint32) ((logSegNo) % XLogSegmentsPerXLogId(wal_segsz_bytes)), \
			 (uint32) (XLogSegmentOffset((startpoint), wal_segsz_bytes)))

#endif							/* XLOG_FNAME_H */
