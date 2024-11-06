/*-------------------------------------------------------------------------
 *
 * undologdesc.c
 *	  rmgr descriptor routines for access/transam/undolog.c
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/undologdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/undolog.h"
#include "catalog/storage.h"
#include "catalog/storage_ulog.h"

typedef struct UndoDescData
{
	const char *rm_name;
	void	(*rm_undodesc) (StringInfo buf, UndoLogRecord *record);
	const char *(*rm_undoidentify) (uint8 info);
} UndoDescData;

#define PG_RMGR(symname,name,redo,desc,identify,startup,cleanup,mask,decode,undo,undo_desc,undo_identify,undo_event) \
	{ name, undo_desc, undo_identify },

static UndoDescData	UndoRoutines[RM_MAX_ID + 1] = {
#include "access/rmgrlist.h"
};
#undef PG_RMGR

void
undolog_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_ULOG_CREATE)
	{
		xl_ulog_create *crec = (xl_ulog_create *) rec;
		char fname[MAXPGPATH];

		UndoLogSetFilename(fname, crec->xid);
		appendStringInfo(buf, "\"%s\"", fname);
	}
	else if (info == XLOG_ULOG_WRITE)
	{
		xl_ulog_write *wrec = (xl_ulog_write *) rec;
		UndoLogRecord *urec = (UndoLogRecord *) wrec->bytes;

		/*
		 * The file header and records are recovered in the same way without
		 * using resource manager routines. However, while description routines
		 * are typically provided as resource routines, the file header does
		 * not have one.  Therefore, it requires explicit handling here.
		 */
		if (wrec->off == 0)
		{
			/* This is the file header. No extra data is currently stored. */
			appendStringInfo(buf, "HEADER");
		}
		else
		{
			/* This is a ulog record. Let rmgr routines handle it. */
			UndoDescData rmgr = UndoRoutines[urec->ul_rmid];
			const char *id = rmgr.rm_undoidentify(ULogRecGetInfo(urec));

			Assert(UndoRoutines[urec->ul_rmid].rm_undoidentify);

			if (id == NULL)
				appendStringInfo(buf, "UNKNOWN (%X): ",
								 ULogRecGetInfo(urec));
			else
				appendStringInfo(buf, "%s: ", id);

			if (UndoRoutines[urec->ul_rmid].rm_undodesc)
				UndoRoutines[urec->ul_rmid].rm_undodesc(buf, urec);
		}
	}
}

const char *
undolog_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_ULOG_CREATE:
			id = "CREATE";
			break;
		case XLOG_ULOG_WRITE:
			id = "WRITE";
			break;
	}

	return id;
}
