/*-------------------------------------------------------------------------
 *
 * storage_ulog.h
 *	  prototypes for Undo Log support for backend/catalog/storage.c
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/storage_ulog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STORAGE_ULOG_H
#define STORAGE_ULOG_H

#include "access/undolog.h"
#include "storage/smgr.h"

/* ULOG gives us high 4 bits (just following xlog) */
#define ULOG_SMGR_CREATE			0x10
#define ULOG_SMGR_PRESERVE			0x20

/* undo log entry for storage file creation */
typedef struct ul_smgr_create
{
	RelFileLocator	rlocator;
	ProcNumber		backend;
	ForkNumber		forknum;
} ul_smgr_create;

typedef struct ul_smgr_preserve
{
	RelFileLocator	rlocator;
	ProcNumber		backend;
	ForkNumber		forknum;
} ul_smgr_preserve;

extern void smgr_undo(UndoLogRecord *record, ULogContext cxt, bool redo,
					  bool crashed);
extern void	smgr_undodesc(StringInfo buf, UndoLogRecord *record);
extern const char *smgr_undoidentify(uint8 info);
extern void smgr_undoevent(ULogEvent event);

#define ULogRecGetData(record) ((char *)record + sizeof(UndoLogRecord))
#define ULogRecGetInfo(record) ((record)->ul_info)

#endif							/* STORAGE_XLOG_H */
