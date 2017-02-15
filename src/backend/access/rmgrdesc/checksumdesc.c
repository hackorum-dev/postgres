/*-------------------------------------------------------------------------
 *
 * checksumdesc.c
 *	  rmgr descriptor routines for commands/checksum.c
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/checksumdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/checksumxlog.h"

void
checksum_desc(StringInfo buf, XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* not sure if we need anything here */
	return;
}

const char *
checksum_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_CHECKSUM_DISABLE:
			id = "DISABLE";
			break;
		case XLOG_CHECKSUM_ENABLE:
			id = "ENABLE";
			break;
		case XLOG_CHECKSUM_ENFORCE:
			id = "ENFORCE";
			break;
		case XLOG_CHECKSUM_REVALIDATE:
			id = "REVALIDATE";
			break;
	}

	return id;
}

