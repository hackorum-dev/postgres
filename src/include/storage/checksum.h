/*-------------------------------------------------------------------------
 *
 * checksum.h
 *	  Checksum implementation for data pages.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/checksum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CHECKSUM_H
#define CHECKSUM_H

#include "storage/block.h"


/*
 * Checksum state indicator.  Note this is stored in pg_control; if you change
 * it, you must bump PG_CONTROL_VERSION
 */
typedef enum ChecksumState
{
	CHECKSUMS_DISABLED = 0,
	CHECKSUMS_ENABLING,
	CHECKSUMS_ENFORCING,
	CHECKSUMS_REVALIDATING
}	ChecksumState;


/*
 * Compute the checksum for a Postgres page.  The page must be aligned on a
 * 4-byte boundary.
 */
extern uint16 pg_checksum_page(char *page, BlockNumber blkno);
extern ChecksumState	data_checksums;

#endif   /* CHECKSUM_H */
