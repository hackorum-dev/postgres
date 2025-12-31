/*-------------------------------------------------------------------------
 *
 * be-fsstubs.h
 *
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/be-fsstubs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BE_FSSTUBS_H
#define BE_FSSTUBS_H

/*
 * These are not fmgr-callable, but are available to C code.
 * Probably these should have had the underscore-free names,
 * but too late now...
 */
extern int	lo_read(int fd, char *buf, int len);
extern int	lo_write(int fd, const char *buf, int len);
extern void lo_put(Oid loOid, int64 offset, const char *str, int len);

struct LoBulkWriteItem;
struct LoBulkPutItem;

extern int64 lo_bulk_write(const struct LoBulkWriteItem *items, int nitems);
extern int64 lo_bulk_put(const struct LoBulkPutItem *items, int nitems);

/*
 * Cleanup LOs at xact commit/abort
 */
extern void AtEOXact_LargeObject(bool isCommit);
extern void AtEOSubXact_LargeObject(bool isCommit, SubTransactionId mySubid,
									SubTransactionId parentSubid);

#endif							/* BE_FSSTUBS_H */
