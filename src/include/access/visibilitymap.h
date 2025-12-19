/*-------------------------------------------------------------------------
 *
 * visibilitymap.h
 *		visibility map interface
 *
 *
 * Portions Copyright (c) 2007-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/visibilitymap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VISIBILITYMAP_H
#define VISIBILITYMAP_H

#include "access/visibilitymapdefs.h"
#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

/* Macros for visibilitymap test */
#define VM_ALL_VISIBLE(r, b, v) \
	((visibilitymap_get_status((r), (b), (v)) & VISIBILITYMAP_ALL_VISIBLE) != 0)
#define VM_ALL_FROZEN(r, b, v) \
	((visibilitymap_get_status((r), (b), (v)) & VISIBILITYMAP_ALL_FROZEN) != 0)

extern bool visibilitymap_clear(Relation rel, BlockNumber heapBlk,
								Buffer vmbuf, uint8 flags);
extern void visibilitymap_pin(Relation rel, BlockNumber heapBlk,
							  Buffer *vmbuf);
extern bool visibilitymap_pin_ok(BlockNumber heapBlk, Buffer vmbuf);
extern uint8 visibilitymap_set(Relation rel,
							   BlockNumber heapBlk, Buffer heapBuf,
							   XLogRecPtr recptr,
							   Buffer vmBuf,
							   TransactionId cutoff_xid,
							   uint8 flags);
extern uint8 visibilitymap_set_vmbits(BlockNumber heapBlk,
									  Buffer vmBuf, uint8 flags,
									  const RelFileLocator rlocator);
extern void visibilitymap_get_statusv(Relation rel, const BlockNumber *heapBlks,
									  uint8 *statusv, int nblocks,
									  Buffer *vmbuf);
extern void visibilitymap_count(Relation rel, BlockNumber *all_visible, BlockNumber *all_frozen);
extern BlockNumber visibilitymap_prepare_truncate(Relation rel,
												  BlockNumber nheapblocks);

static inline uint8
visibilitymap_get_status(Relation rel, BlockNumber heapBlk, Buffer *vmbuf)
{
	uint8	status;

	visibilitymap_get_statusv(rel, &heapBlk, &status, 1, vmbuf);

	return status;
}

#endif							/* VISIBILITYMAP_H */
