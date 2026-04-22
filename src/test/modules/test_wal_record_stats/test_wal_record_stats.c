/*-------------------------------------------------------------------------
 *
 * test_wal_record_stats.c
 *		Test module exercising WALReadFromBuffers() to read WAL records
 *		directly from WAL buffers (shared memory, no disk I/O).
 *
 * Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_wal_record_stats/test_wal_record_stats.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecovery.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC;

/*
 * page_read callback that reads WAL directly from WAL buffers.
 */
static int
wal_buffer_page_read(XLogReaderState *state, XLogRecPtr targetPagePtr,
					 int reqLen, XLogRecPtr targetRecPtr,
					 char *cur_page)
{
	XLogRecPtr	read_upto;
	XLogRecPtr	loc;
	TimeLineID	tli = GetWALInsertionTimeLine();
	Size		count;
	Size		nbytes;

	loc = targetPagePtr + reqLen;

	read_upto = GetXLogInsertRecPtr();

	/*
	 * If the requested WAL hasn't been inserted yet, return -1 rather than
	 * waiting.  The WAL between start_lsn and end_lsn should already be
	 * inserted by the time we're called.
	 */
	if (loc > read_upto)
		return -1;

	/* Ensure any in-progress insertions up to this point are visible */
	WaitXLogInsertionsToFinish(loc);

	if (targetPagePtr + XLOG_BLCKSZ <= read_upto)
		count = XLOG_BLCKSZ;
	else if (targetPagePtr + reqLen > read_upto)
		return -1;
	else
		count = read_upto - targetPagePtr;

	nbytes = WALReadFromBuffers(cur_page, targetPagePtr, count, tli);

	if (nbytes <= 0)
		return -1;		/* data evicted from circular WAL buffer */

	return nbytes;
}

/* Per-rmgr/record_type accumulation entry. */
typedef struct WalRecordStat
{
	char		resource_manager[64];
	char		record_type[64];
	int64		count;
	int64		total_record_length;
	int64		total_main_data_length;
	int64		total_fpi_length;
} WalRecordStat;

#define MAX_WAL_STAT_ENTRIES 256

/*
 * get_wal_record_stats_from_buffers(start_lsn, end_lsn)
 *
 * Returns per-resource_manager/record_type WAL record statistics by reading
 * directly from WAL buffers via WALReadFromBuffers().
 */
PG_FUNCTION_INFO_V1(get_wal_record_stats_from_buffers);
Datum
get_wal_record_stats_from_buffers(PG_FUNCTION_ARGS)
{
#define WAL_RECORD_STATS_COLS 6
	XLogRecPtr	start_lsn = PG_GETARG_LSN(0);
	XLogRecPtr	end_lsn = PG_GETARG_LSN(1);
	XLogReaderState *xlogreader;
	XLogRecPtr	first_valid_record;
	char	   *errormsg;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	WalRecordStat stats[MAX_WAL_STAT_ENTRIES];
	int			nstats = 0;
	int			i;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("WAL buffers can only be read on a primary server")));

	if (start_lsn < XLOG_BLCKSZ)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not read WAL at LSN %X/%08X",
						LSN_FORMAT_ARGS(start_lsn))));

	if (start_lsn > end_lsn)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("WAL start LSN must be less than end LSN")));

	memset(stats, 0, sizeof(stats));

	InitMaterializedSRF(fcinfo, 0);

	xlogreader = XLogReaderAllocate(wal_segment_size, NULL,
									XL_ROUTINE(.page_read = &wal_buffer_page_read,
											   .segment_open = NULL,
											   .segment_close = NULL),
									NULL);

	if (xlogreader == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));

	first_valid_record = XLogFindNextRecord(xlogreader, start_lsn, &errormsg);

	if (XLogRecPtrIsInvalid(first_valid_record))
	{
		XLogReaderFree(xlogreader);
		PG_RETURN_VOID();
	}

	/* Scan WAL records and accumulate stats */
	while (true)
	{
		XLogRecord *record;
		RmgrData	desc;
		const char *rmgr_name;
		const char *rec_type;
		int			found = -1;

		record = XLogReadRecord(xlogreader, &errormsg);
		if (record == NULL)
			break;

		if (xlogreader->EndRecPtr > end_lsn)
			break;

		desc = GetRmgr(XLogRecGetRmid(xlogreader));
		rmgr_name = desc.rm_name;
		rec_type = desc.rm_identify(XLogRecGetInfo(xlogreader));
		if (rec_type == NULL)
			rec_type = "UNKNOWN";

		/* Find existing entry or create new one */
		for (i = 0; i < nstats; i++)
		{
			if (strcmp(stats[i].resource_manager, rmgr_name) == 0 &&
				strcmp(stats[i].record_type, rec_type) == 0)
			{
				found = i;
				break;
			}
		}

		if (found < 0)
		{
			if (nstats >= MAX_WAL_STAT_ENTRIES)
			{
				ereport(WARNING,
						(errmsg("WAL record stat entries limit reached (%d)",
								MAX_WAL_STAT_ENTRIES)));
				break;
			}
			found = nstats++;
			strlcpy(stats[found].resource_manager, rmgr_name,
					sizeof(stats[found].resource_manager));
			strlcpy(stats[found].record_type, rec_type,
					sizeof(stats[found].record_type));
		}

		stats[found].count++;
		stats[found].total_record_length += XLogRecGetTotalLen(xlogreader);
		stats[found].total_main_data_length += XLogRecGetDataLen(xlogreader);

		if (XLogRecHasAnyBlockRefs(xlogreader))
		{
			uint32		fpi_len = 0;
			StringInfoData dummy;

			initStringInfo(&dummy);
			XLogRecGetBlockRefInfo(xlogreader, false, false, &dummy, &fpi_len);
			pfree(dummy.data);
			stats[found].total_fpi_length += fpi_len;
		}

		CHECK_FOR_INTERRUPTS();
	}

	XLogReaderFree(xlogreader);

	/* Emit result rows */
	for (i = 0; i < nstats; i++)
	{
		Datum		values[WAL_RECORD_STATS_COLS] = {0};
		bool		nulls[WAL_RECORD_STATS_COLS] = {0};
		int			col = 0;

		values[col++] = CStringGetTextDatum(stats[i].resource_manager);
		values[col++] = CStringGetTextDatum(stats[i].record_type);
		values[col++] = Int64GetDatum(stats[i].count);
		values[col++] = Int64GetDatum(stats[i].total_record_length);
		values[col++] = Int64GetDatum(stats[i].total_main_data_length);
		values[col++] = Int64GetDatum(stats[i].total_fpi_length);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	PG_RETURN_VOID();

#undef WAL_RECORD_STATS_COLS
}
