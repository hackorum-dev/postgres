/*-------------------------------------------------------------------------
 *
 * gistdesc.c
 *	  rmgr descriptor routines for access/gist/gistxlog.c
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/gistdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gistxlog.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"

static void
out_gistxlogPageUpdate(StringInfo buf, XLogReaderState *record, gistxlogPageUpdate *xlrec)
{
	appendStringInfo(buf, "ntodelete %u ntoinsert %u",
				xlrec->ntodelete, xlrec->ntoinsert);

	if (XLogRecHasBlockData(record, 0)) {
		char* payload = XLogRecGetBlockData(record, 0, NULL);
		OffsetNumber *todelete = (OffsetNumber *) payload;
		uint16 i;

		if (xlrec->ntodelete) {
			appendStringInfo(buf, " delete offsets: ");

			for (i = 0; i < xlrec->ntodelete; ++i) {
				if (i + 1 != xlrec->ntodelete)
					appendStringInfo(buf, "%d, ", todelete[i]);
				else
					appendStringInfo(buf, "%d", todelete[i]);
			}

			payload += sizeof(OffsetNumber) * xlrec->ntodelete;
		}
	}
}

static void
out_gistxlogPageReuse(StringInfo buf, gistxlogPageReuse *xlrec)
{
	appendStringInfo(buf, "rel %u/%u/%u; blk %u; snapshotConflictHorizon %u:%u, isCatalogRel %c",
					 xlrec->locator.spcOid, xlrec->locator.dbOid,
					 xlrec->locator.relNumber, xlrec->block,
					 EpochFromFullTransactionId(xlrec->snapshotConflictHorizon),
					 XidFromFullTransactionId(xlrec->snapshotConflictHorizon),
					 xlrec->isCatalogRel ? 'T' : 'F');
}

static void
out_gistxlogDelete(StringInfo buf, gistxlogDelete *xlrec)
{
	appendStringInfo(buf, "delete: snapshotConflictHorizon %u, nitems: %u, isCatalogRel %c",
					 xlrec->snapshotConflictHorizon, xlrec->ntodelete,
					 xlrec->isCatalogRel ? 'T' : 'F');
}

static void
out_gistxlogPageSplit(StringInfo buf, XLogReaderState *record, gistxlogPageSplit *xlrec)
{
	int i;
	appendStringInfo(buf, "page_split: splits to %d pages, origrlink %d, origleaf %c, orignsn: %ld, markfollowright: %c",
					 xlrec->npage, xlrec->origrlink,
					 xlrec->origleaf ? 'T' : 'F', xlrec->orignsn,
					 xlrec->markfollowright ? 'T' : 'F');

	for (i = 1; i <= xlrec->npage; ++ i)
	{
		int n;

		/* extract the number of tuples */
		memcpy(&n, XLogRecGetBlockData(record, i, NULL), sizeof(int));
		appendStringInfo(buf, ", blk data %d: adds %d tuples",
					 i, n);
	}
}

static void
out_gistxlogPageDelete(StringInfo buf, gistxlogPageDelete *xlrec)
{
	appendStringInfo(buf, "deleteXid %u:%u; downlink %u",
					 EpochFromFullTransactionId(xlrec->deleteXid),
					 XidFromFullTransactionId(xlrec->deleteXid),
					 xlrec->downlinkOffset);
}

void
gist_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_GIST_PAGE_UPDATE:
			out_gistxlogPageUpdate(buf, record, (gistxlogPageUpdate *) rec);
			break;
		case XLOG_GIST_PAGE_REUSE:
			out_gistxlogPageReuse(buf, (gistxlogPageReuse *) rec);
			break;
		case XLOG_GIST_DELETE:
			out_gistxlogDelete(buf, (gistxlogDelete *) rec);
			break;
		case XLOG_GIST_PAGE_SPLIT:
			out_gistxlogPageSplit(buf, record, (gistxlogPageSplit *) rec);
			break;
		case XLOG_GIST_PAGE_DELETE:
			out_gistxlogPageDelete(buf, (gistxlogPageDelete *) rec);
			break;
	}
}

const char *
gist_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_GIST_PAGE_UPDATE:
			id = "PAGE_UPDATE";
			break;
		case XLOG_GIST_DELETE:
			id = "DELETE";
			break;
		case XLOG_GIST_PAGE_REUSE:
			id = "PAGE_REUSE";
			break;
		case XLOG_GIST_PAGE_SPLIT:
			id = "PAGE_SPLIT";
			break;
		case XLOG_GIST_PAGE_DELETE:
			id = "PAGE_DELETE";
			break;
	}

	return id;
}
