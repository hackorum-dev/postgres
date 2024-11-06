/*-------------------------------------------------------------------------
 *
 * smgrundodesc.c
 *	  rmgr undolog descriptor routines for catalog/storage.c
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/smgrundodesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "catalog/storage_ulog.h"
#include "lib/stringinfo.h"

void
smgr_undodesc(StringInfo buf, UndoLogRecord *record)
{
	uint8		info = ULogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == ULOG_SMGR_CREATE)
	{
		ul_smgr_create *urec = (ul_smgr_create *) ULogRecGetData(record);

		appendStringInfo(buf, ": %d/%d/%d, fork %d, backend %d",
						 urec->rlocator.spcOid,
						 urec->rlocator.dbOid,
						 urec->rlocator.relNumber,
						 urec->forknum, urec->backend);
	}
	else if (info == ULOG_SMGR_PRESERVE)
	{
		ul_smgr_preserve *urec = (ul_smgr_preserve *) ULogRecGetData(record);

		appendStringInfo(buf, ": %d/%d/%d, fork %d, backend %d",
						 urec->rlocator.spcOid,
						 urec->rlocator.dbOid,
						 urec->rlocator.relNumber,
						 urec->forknum, urec->backend);
	}
}

const char *
smgr_undoidentify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case ULOG_SMGR_CREATE:
			id = "SMGRCREATE";
			break;
		case ULOG_SMGR_PRESERVE:
			id = "SMGRPRESERVE";
			break;
	}

	return id;
}
