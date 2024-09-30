/*-------------------------------------------------------------------------
 *
 * undolog.h
 *	  Definitions for undolog module of PostgresSQL
 *
 * Copyright (c) 2000-2024, PostgreSQL Global Development Group
 *
 * src/include/access/undolog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNDOLOG_H
#define UNDOLOG_H

#include "access/transam.h"
#include "access/xlogreader.h"

/* Directory for storing undo logs */
#define UNDOLOG_DIR	"pg_ulog"

typedef struct UndoLogFileHeader
{
	int32 magic;			/* fixed ULOG file magic number */
	bool  crashed;			/* this transaction experienced a crash */
	/* UndoLogRecord follows */
} UndoLogFileHeader;

typedef struct UndoLogRecord
{
	uint32		ul_tot_len;		/* total length of entire record */
	RmgrId		ul_rmid;		/* resource manager for this record */
	uint8		ul_info;		/* record info */
	FullTransactionId ul_xid;	/* subtransaction id */
	/* rmgr-specific data follow, no padding */
} UndoLogRecord;

/*
 * The high 4 bits in ul_info may be used freely by rmgr. The lower 4 bits are
 * not used for now.
 */
#define ULR_INFO_MASK			0x0F
#define ULR_RMGR_INFO_MASK		0xF0

/* XLOG stuff */
#define XLOG_ULOG_CREATE		0x00
#define XLOG_ULOG_WRITE			0x10

typedef struct xl_ulog_create
{
	FullTransactionId	xid;
} xl_ulog_create;

typedef struct xl_ulog_write
{
	FullTransactionId	topxid;
	FullTransactionId	subxid;
	off_t	off;
	Size	len;
	unsigned char bytes[FLEXIBLE_ARRAY_MEMBER];
} xl_ulog_write;

extern void undolog_redo(XLogReaderState *record);
extern void undolog_desc(StringInfo buf, XLogReaderState *record);
extern const char *undolog_identify(uint8 info);

#define ULogRecGetData(record) ((char *)record + sizeof(UndoLogRecord))
#define ULogRecGetInfo(record) ((record)->ul_info)

/*
 * UndoLogSetFilename()
 *
 * Generates undo log file name for the xid. Used in undolog.c and
 * undologdesc.c.
 */
static inline void
UndoLogSetFilename(char *buf, FullTransactionId xid)
{
	StaticAssertDecl(sizeof(FullTransactionId) == 8,
					 "width of FullTrasactionId is not 8");
	snprintf(buf, MAXPGPATH, "%s/%016" PRIx64,
			 UNDOLOG_DIR, U64FromFullTransactionId(xid));
}

#endif							/* UNDOLOG_H */
