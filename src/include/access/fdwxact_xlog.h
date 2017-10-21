/*-------------------------------------------------------------------------
 *
 * fdwxact_xlog.h
 *	  Foreign transaction XLOG definitions.
 *
 *
 * Portions Copyright (c) 2018, PostgreSQL Global Development Group
 *
 * src/include/access/fdwxact_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FDWXACT_XLOG_H
#define FDWXACT_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"

/* Info types for logs related to FDW transactions */
#define XLOG_FDW_XACT_INSERT	0x00
#define XLOG_FDW_XACT_REMOVE	0x10

#define FDW_XACT_ID_LEN (2 + 1 + 8 + 1 + 8 + 1 + 8)
/*
 * On disk file structure
 */
typedef struct
{
	Oid			dboid;			/* database oid where to find foreign server
								 * and user mapping */
	TransactionId local_xid;
	Oid			serverid;		/* foreign server where transaction takes
								 * place */
	Oid			userid;			/* user who initiated the foreign transaction */
	Oid			umid;			/* user mapping oid */
	char		fdw_xact_id[FDW_XACT_ID_LEN]; /* foreign txn prepare id */
}	FdwXactOnDiskData;

typedef struct
{
	TransactionId xid;
	Oid			serverid;
	Oid			userid;
	Oid			dbid;
}	FdwRemoveXlogRec;

extern void fdw_xact_redo(XLogReaderState *record);
extern void fdw_xact_desc(StringInfo buf, XLogReaderState *record);
extern const char *fdw_xact_identify(uint8 info);

#endif	/* FDWXACT_XLOG_H */
