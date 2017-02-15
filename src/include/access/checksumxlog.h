/*-------------------------------------------------------------------------
 *
 * checksumxlog.h
 *	  checksum xlog routines
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/checksumxlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CHECKSUM_XLOG_H
#define CHECKSUM_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"

#define XLOG_CHECKSUM_DISABLE		0x00
#define XLOG_CHECKSUM_ENABLE		0x10
#define XLOG_CHECKSUM_ENFORCE		0x20
#define XLOG_CHECKSUM_REVALIDATE	0x30

extern void checksum_redo(XLogReaderState *record);
extern void checksum_desc(StringInfo buf, XLogReaderState *record);
extern const char *checksum_identify(uint8 info);

#endif
