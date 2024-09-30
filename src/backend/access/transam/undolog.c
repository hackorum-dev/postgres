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

#include "access/undolog.h"

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
	}
	else if (info == XLOG_ULOG_WRITE)
	{
	}
}
