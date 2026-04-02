/* -------------------------------------------------------------------------
 *
 * pgstat_tablespace.c
 *	  Implementation of tablespace statistics.
 *
 * Copyright (c) 2001-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_tablespace.c
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"


/*
 * Remove entry for the tablespace being dropped.
 */
void
pgstat_drop_tablespace(Oid tablespaceid)
{
	pgstat_drop_transactional(PGSTAT_KIND_TABLESPACE, InvalidOid, tablespaceid);
}

/*
 * Fetch tablespace statistics.
 */
PgStat_StatTabspaceEntry *
pgstat_fetch_stat_tabspaceentry(Oid tablespaceid)
{
	return (PgStat_StatTabspaceEntry *)
		pgstat_fetch_entry(PGSTAT_KIND_TABLESPACE, InvalidOid, tablespaceid);
}

/*
 * Flush out pending stats for the entry.
 */
bool
pgstat_tablespace_flush_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	PgStatShared_Tablespace *sharedent;
	PgStat_StatTabspaceEntry *pendingent;

	pendingent = (PgStat_StatTabspaceEntry *) entry_ref->pending;
	sharedent = (PgStatShared_Tablespace *) entry_ref->shared_stats;

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

#define PGSTAT_ACCUM_TABSPACECOUNT(item)		\
	(sharedent)->stats.item += (pendingent)->item

	PGSTAT_ACCUM_TABSPACECOUNT(blocks_fetched);
	PGSTAT_ACCUM_TABSPACECOUNT(blocks_hit);
	PGSTAT_ACCUM_TABSPACECOUNT(blk_read_time);
	PGSTAT_ACCUM_TABSPACECOUNT(blk_write_time);
	PGSTAT_ACCUM_TABSPACECOUNT(temp_files);
	PGSTAT_ACCUM_TABSPACECOUNT(temp_bytes);
	PGSTAT_ACCUM_TABSPACECOUNT(tuples_returned);
	PGSTAT_ACCUM_TABSPACECOUNT(tuples_fetched);
	PGSTAT_ACCUM_TABSPACECOUNT(tuples_inserted);
	PGSTAT_ACCUM_TABSPACECOUNT(tuples_updated);
	PGSTAT_ACCUM_TABSPACECOUNT(tuples_deleted);

#undef PGSTAT_ACCUM_TABSPACECOUNT

	pgstat_unlock_entry(entry_ref);

	/* Clear pending stats since they have been flushed */
	memset(pendingent, 0, sizeof(*pendingent));

	return true;
}

/*
 * Reset stats reset timestamp.
 */
void
pgstat_tablespace_reset_timestamp_cb(PgStatShared_Common *header, TimestampTz ts)
{
	((PgStatShared_Tablespace *) header)->stats.stat_reset_timestamp = ts;
}

/*
 * Prepare for reporting tablespace stats.
 */
PgStat_StatTabspaceEntry *
pgstat_prep_tablespace_pending(Oid tablespaceid)
{
	PgStat_EntryRef *entry_ref;

	Assert(OidIsValid(tablespaceid));

	entry_ref = pgstat_prep_pending_entry(PGSTAT_KIND_TABLESPACE,
										  InvalidOid, tablespaceid, NULL);

	return (PgStat_StatTabspaceEntry *) entry_ref->pending;
}

/*
 * Count tablespace buffer write time.
 */
void
pgstat_count_tablespace_buffer_write_time(uint64 duration, Oid tablespace_oid)
{
	if (OidIsValid(tablespace_oid))
	{
		PgStat_StatTabspaceEntry *tsent = pgstat_prep_tablespace_pending(tablespace_oid);
		tsent->blk_write_time += duration;
	}
}

/*
 * Count tablespace buffer read time.
 */
void
pgstat_count_tablespace_buffer_read_time(uint64 duration, Oid tablespace_oid)
{
	if (OidIsValid(tablespace_oid))
	{
		PgStat_StatTabspaceEntry *tsent = pgstat_prep_tablespace_pending(tablespace_oid);

		tsent->blk_read_time += duration;
	}
}
