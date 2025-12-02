/*-------------------------------------------------------------------------
 *
 * md.h
 *	  magnetic disk storage manager public interface declarations.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/md.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MD_H
#define MD_H

#include "storage/aio_types.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"
#include "storage/smgr.h"
#include "storage/sync.h"

extern PGDLLIMPORT const PgAioHandleCallbacks aio_md_readv_cb;

/*
 * Hook function types for I/O transformation (e.g., encryption/decryption).
 * These hooks allow extensions to transform data during storage I/O operations.
 */

/*
 * Called after blocks are read from disk, before PostgreSQL's checksum verification.
 * Extension can reverse-transform (e.g., decrypt) the data in place.
 *
 * For synchronous reads, called from mdreadv() after read completes.
 * For AIO reads, called from buffer_readv_complete_one() before PageIsVerified().
 *
 * Note: The hook is responsible for verifying on-disk checksum before reverse
 * transformation and recalculating checksum after transformation. This ensures
 * data integrity is verified at both stages and PostgreSQL's checksum verification
 * passes.
 *
 * On failure, the hook should raise an ERROR (or PANIC for critical errors).
 */
typedef void (*mdread_post_hook_type) (RelFileLocator *rlocator,
									   ForkNumber forknum,
									   BlockNumber blocknum,
									   void **buffers,
									   BlockNumber nblocks);

/*
 * Called before mdwritev() writes blocks to disk.
 * Extension can transform (e.g., encrypt) data.
 * Returns pointer to transformed buffers array (hook manages the memory,
 * typically using static local storage).
 *
 * Note: The hook should recalculate checksum on transformed data after
 * transformation. This on-disk checksum will be verified on read before
 * reverse transformation, ensuring disk-level data integrity.
 *
 * On failure, the hook should raise an ERROR (or PANIC for critical errors),
 * or return the original buffers with a WARNING as fallback.
 */
typedef const void **(*mdwrite_pre_hook_type) (RelFileLocator *rlocator,
											   ForkNumber forknum,
											   BlockNumber blocknum,
											   const void **buffers,
											   BlockNumber nblocks);

/*
 * Called before mdextend() extends a relation with new blocks.
 * Returns pointer to transformed buffer (hook manages the memory,
 * typically using static local storage).
 *
 * Note: Same as write hook - the hook should recalculate checksum on
 * transformed data after transformation.
 *
 * On failure, the hook should raise an ERROR (or PANIC for critical errors),
 * or return the original buffer with a WARNING as fallback.
 */
typedef const void *(*mdextend_pre_hook_type) (RelFileLocator *rlocator,
											   ForkNumber forknum,
											   BlockNumber blocknum,
											   const void *buffer);

/* Hook variables for I/O transformation */
extern PGDLLIMPORT mdread_post_hook_type mdread_post_hook;
extern PGDLLIMPORT mdwrite_pre_hook_type mdwrite_pre_hook;
extern PGDLLIMPORT mdextend_pre_hook_type mdextend_pre_hook;

/* md storage manager functionality */
extern void mdinit(void);
extern void mdopen(SMgrRelation reln);
extern void mdclose(SMgrRelation reln, ForkNumber forknum);
extern void mdcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo);
extern bool mdexists(SMgrRelation reln, ForkNumber forknum);
extern void mdunlink(RelFileLocatorBackend rlocator, ForkNumber forknum, bool isRedo);
extern void mdextend(SMgrRelation reln, ForkNumber forknum,
					 BlockNumber blocknum, const void *buffer, bool skipFsync);
extern void mdzeroextend(SMgrRelation reln, ForkNumber forknum,
						 BlockNumber blocknum, int nblocks, bool skipFsync);
extern bool mdprefetch(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber blocknum, int nblocks);
extern uint32 mdmaxcombine(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber blocknum);
extern void mdreadv(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
					void **buffers, BlockNumber nblocks);
extern void mdstartreadv(PgAioHandle *ioh,
						 SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
						 void **buffers, BlockNumber nblocks);
extern void mdwritev(SMgrRelation reln, ForkNumber forknum,
					 BlockNumber blocknum,
					 const void **buffers, BlockNumber nblocks, bool skipFsync);
extern void mdwriteback(SMgrRelation reln, ForkNumber forknum,
						BlockNumber blocknum, BlockNumber nblocks);
extern BlockNumber mdnblocks(SMgrRelation reln, ForkNumber forknum);
extern void mdtruncate(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber curnblk, BlockNumber nblocks);
extern void mdimmedsync(SMgrRelation reln, ForkNumber forknum);
extern void mdregistersync(SMgrRelation reln, ForkNumber forknum);
extern int	mdfd(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, uint32 *off);

extern void ForgetDatabaseSyncRequests(Oid dbid);
extern void DropRelationFiles(RelFileLocator *delrels, int ndelrels, bool isRedo);

/* md sync callbacks */
extern int	mdsyncfiletag(const FileTag *ftag, char *path);
extern int	mdunlinkfiletag(const FileTag *ftag, char *path);
extern bool mdfiletagmatches(const FileTag *ftag, const FileTag *candidate);

#endif							/* MD_H */
