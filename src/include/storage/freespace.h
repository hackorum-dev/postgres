/*-------------------------------------------------------------------------
 *
 * freespace.h
 *	  POSTGRES free space map for quickly finding free space in relations
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/freespace.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FREESPACE_H_
#define FREESPACE_H_

#include "storage/block.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

typedef enum FreeSpaceStrategy
{
	FREESPACE_STRATEGY_MAX_CONCURRENCY = 0,
		/*
		 * Each time we ask for a new block with freespace this will set
		 * the advancenext flag which increments the next block by one.
		 * The effect of this is to ensure that all backends are given
		 * a separate block, minimizing block contention and thereby
		 * maximising concurrency. This is the default strategy used by
		 * PostgreSQL since at least PostgreSQL 8.4.
		 */

	FREESPACE_STRATEGY_MAX_COMPACT
		/*
		 * All backends are given the earliest block in the table with
		 * sufficient freespace for the insert. This could cause block
		 * contention for concurrent inserts, but ensures maximum data
		 * compaction, which will then allow vacuum truncation to release
		 * as much space as possible.  This strategy may be appropriate
		 * for short periods if a table becomes bloated.
		 */

} FreeSpaceStrategy;

/* prototypes for public functions in freespace.c */
extern Size GetRecordedFreeSpace(Relation rel, BlockNumber heapBlk);
extern BlockNumber GetPageWithFreeSpace(Relation rel, Size spaceNeeded);
extern BlockNumber GetPageWithFreeSpaceExt(Relation rel, Size spaceNeeded,
											FreeSpaceStrategy fss);
extern BlockNumber RecordAndGetPageWithFreeSpace(Relation rel,
												 BlockNumber oldPage,
												 Size oldSpaceAvail,
												 Size spaceNeeded);
extern BlockNumber RecordAndGetPageWithFreeSpaceExt(Relation rel,
												 BlockNumber oldPage,
												 Size oldSpaceAvail,
												 Size spaceNeeded,
												 FreeSpaceStrategy fss);
extern void RecordPageWithFreeSpace(Relation rel, BlockNumber heapBlk,
									Size spaceAvail);
extern void RecordPageWithFreeSpaceExt(Relation rel, BlockNumber heapBlk,
									Size spaceAvail, FreeSpaceStrategy fss);
extern void XLogRecordPageWithFreeSpace(RelFileLocator rlocator, BlockNumber heapBlk,
										Size spaceAvail);

extern BlockNumber FreeSpaceMapPrepareTruncateRel(Relation rel,
												  BlockNumber nblocks);
extern void FreeSpaceMapVacuum(Relation rel);
extern void FreeSpaceMapVacuumRange(Relation rel, BlockNumber start,
									BlockNumber end);

#endif							/* FREESPACE_H_ */
