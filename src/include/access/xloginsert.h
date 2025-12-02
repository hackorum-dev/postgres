/*
 * xloginsert.h
 *
 * Functions for generating WAL records
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xloginsert.h
 */
#ifndef XLOGINSERT_H
#define XLOGINSERT_H

#include "access/rmgr.h"
#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

/* Forward declaration for XLogRecData */
struct XLogRecData;

/*
 * Hook function type for WAL insert transformation (e.g., encryption).
 * Called after XLogRecordAssemble() but before XLogInsertRecord().
 * Extension can transform the assembled WAL record data for encryption.
 * Returns the (possibly modified) XLogRecData chain to be inserted.
 *
 * The first node's data points to XLogRecord header, which contains
 * xl_rmid and xl_info if needed by the hook.
 *
 * On failure, the hook should either PANIC or return the original rdata
 * as fallback.
 */
typedef struct XLogRecData *(*xlog_insert_pre_hook_type) (struct XLogRecData *rdata);

/* Hook variable for WAL insert transformation */
extern PGDLLIMPORT xlog_insert_pre_hook_type xlog_insert_pre_hook;

/*
 * The minimum size of the WAL construction working area. If you need to
 * register more than XLR_NORMAL_MAX_BLOCK_ID block references or have more
 * than XLR_NORMAL_RDATAS data chunks in a single WAL record, you must call
 * XLogEnsureRecordSpace() first to allocate more working memory.
 */
#define XLR_NORMAL_MAX_BLOCK_ID		4
#define XLR_NORMAL_RDATAS			20

/* flags for XLogRegisterBuffer */
#define REGBUF_FORCE_IMAGE	0x01	/* force a full-page image */
#define REGBUF_NO_IMAGE		0x02	/* don't take a full-page image */
#define REGBUF_WILL_INIT	(0x04 | 0x02)	/* page will be re-initialized at
											 * replay (implies NO_IMAGE) */
#define REGBUF_STANDARD		0x08	/* page follows "standard" page layout,
									 * (data between pd_lower and pd_upper
									 * will be skipped) */
#define REGBUF_KEEP_DATA	0x10	/* include data even if a full-page image
									 * is taken */
#define REGBUF_NO_CHANGE	0x20	/* intentionally register clean buffer */

/* prototypes for public functions in xloginsert.c: */
extern void XLogBeginInsert(void);
extern void XLogSetRecordFlags(uint8 flags);
extern XLogRecPtr XLogInsert(RmgrId rmid, uint8 info);
extern XLogRecPtr XLogSimpleInsertInt64(RmgrId rmid, uint8 info, int64 value);
extern void XLogEnsureRecordSpace(int max_block_id, int ndatas);
extern void XLogRegisterData(const void *data, uint32 len);
extern void XLogRegisterBuffer(uint8 block_id, Buffer buffer, uint8 flags);
extern void XLogRegisterBlock(uint8 block_id, RelFileLocator *rlocator,
							  ForkNumber forknum, BlockNumber blknum, const PageData *page,
							  uint8 flags);
extern void XLogRegisterBufData(uint8 block_id, const void *data, uint32 len);
extern void XLogResetInsertion(void);
extern bool XLogCheckBufferNeedsBackup(Buffer buffer);

extern XLogRecPtr log_newpage(RelFileLocator *rlocator, ForkNumber forknum,
							  BlockNumber blkno, Page page, bool page_std);
extern void log_newpages(RelFileLocator *rlocator, ForkNumber forknum, int num_pages,
						 BlockNumber *blknos, Page *pages, bool page_std);
extern XLogRecPtr log_newpage_buffer(Buffer buffer, bool page_std);
extern void log_newpage_range(Relation rel, ForkNumber forknum,
							  BlockNumber startblk, BlockNumber endblk, bool page_std);
extern XLogRecPtr XLogSaveBufferForHint(Buffer buffer, bool buffer_std);

extern void InitXLogInsert(void);

#endif							/* XLOGINSERT_H */
