/* -------------------------------------------------------------------------
 *
 * pgstat_waitevent.c
 *	  Implementation of wait event statistics.
 *
 * This file contains the implementation of wait event statistics. It is kept
 * separate from pgstat.c to enforce the line between the statistics access /
 * storage implementation and the details about individual types of
 * statistics.
 *
 * Copyright (c) 2001-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_waitevent.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"

bool		have_wait_event_stats = false;

static PgStat_PendingWaitevent PendingWaitEventStats;

/*
 * Support function for the SQL-callable pgstat* functions. Returns
 * a pointer to the wait events statistics struct.
 */
PgStat_WaitEvent *
pgstat_fetch_stat_wait_event(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_WAIT_EVENT);

	return &pgStatLocal.snapshot.wait_event;
}

/*
 * Returns a pointer to the first counter for a specific class.
 */
static PgStat_Counter *
waitEventGetClassCounters(int64 *waitEventStats, int classId)
{
	int			offset = WaitClassTable[classId].offSet;

	return &waitEventStats[offset];
}

/*
 * Returns a pointer to the counter for a specific wait event.
 */
static PgStat_Counter *
waitEventGetCounter(int64 *waitEventStats, int classId, int eventId)
{
	int64	   *classCounters;

	Assert(classId >= 0 && classId < NB_WAITCLASSTABLE_ENTRIES);
	Assert(eventId >= 0 && eventId < WaitClassTable[classId].numberOfEvents);

	classCounters = waitEventGetClassCounters(waitEventStats, classId);

	return &classCounters[eventId];
}

/*
 * Increment a wait event stat counter.
 */
inline void
waitEventIncrementCounter(uint32 wait_event_info)
{
	DecodedWaitInfo waitInfo;
	PgStat_Counter *counter;
	uint32		classId;
	uint16		eventId;

	classId = *my_wait_event_info & WAIT_EVENT_CLASS_MASK;
	eventId = *my_wait_event_info & WAIT_EVENT_ID_MASK;

	if (classId == 0 && eventId == 0)
		return;

	/* Don't take into account user defined LWLock in the stats */
	if (classId == PG_WAIT_LWLOCK && eventId >= LWTRANCHE_FIRST_USER_DEFINED)
		return;

	/* Don't take into account custom wait event extension in the stats */
	if (classId == PG_WAIT_EXTENSION && eventId >= WAIT_EVENT_CUSTOM_INITIAL_ID)
		return;

	/* Don't take account PG_WAIT_INJECTIONPOINT */
	if (classId == PG_WAIT_INJECTIONPOINT)
		return;

	WAIT_EVENT_INFO_DECODE(waitInfo, wait_event_info);

	counter = waitEventGetCounter(PendingWaitEventStats.counts, waitInfo.classId,
								  waitInfo.eventId);

	(*counter)++;

	have_wait_event_stats = true;
}

const char *
get_wait_event_name_from_index(int index)
{
	/* Iterate through the WaitClassTable */
	for (int classIdx = 0; classIdx < NB_WAITCLASSTABLE_ENTRIES; classIdx++)
	{
		int			classOffset = WaitClassTable[classIdx].offSet;
		int			classSize = WaitClassTable[classIdx].numberOfEvents;

		/* Skip empty entries */
		if (WaitClassTable[classIdx].numberOfEvents == 0)
			continue;

		/* Check if the index falls within this class section */
		if (index >= classOffset && index < classOffset + classSize)
		{
			/* Calculate the event ID within this class */
			int			eventId = index - classOffset;

			return WaitClassTable[classIdx].eventNames[eventId];
		}
	}

	Assert(false);
	return "unknown";
}

/*
 * Flush out locally pending wait event statistics
 *
 * If no stats have been recorded, this function returns false.
 *
 * If nowait is true, this function returns true if the lock could not be
 * acquired. Otherwise, return false.
 */
bool
pgstat_wait_event_flush_cb(bool nowait)
{
	PgStatShared_WaitEvent *stats_shmem = &pgStatLocal.shmem->wait_event;
	int			i;

	if (!have_wait_event_stats)
		return false;

	if (!nowait)
		LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	else if (!LWLockConditionalAcquire(&stats_shmem->lock, LW_EXCLUSIVE))
		return true;

	for (i = 0; i < NB_WAITCLASSTABLE_SIZE; i++)
	{
		PgStat_WaitEvent *sharedent = &stats_shmem->stats;

		sharedent->counts[i] += PendingWaitEventStats.counts[i];
	}

	/* done, clear the pending entry */
	MemSet(PendingWaitEventStats.counts, 0, sizeof(PendingWaitEventStats.counts));

	LWLockRelease(&stats_shmem->lock);

	have_wait_event_stats = false;

	return false;
}

void
pgstat_wait_event_init_shmem_cb(void *stats)
{
	PgStatShared_WaitEvent *stat_shmem = (PgStatShared_WaitEvent *) stats;

	LWLockInitialize(&stat_shmem->lock, LWTRANCHE_PGSTATS_DATA);
}

void
pgstat_wait_event_reset_all_cb(TimestampTz ts)
{
	for (int i = 0; i < NB_WAITCLASSTABLE_SIZE; i++)
	{
		LWLock	   *stats_lock = &pgStatLocal.shmem->wait_event.lock;
		PgStat_Counter *counters = &pgStatLocal.shmem->wait_event.stats.counts[i];

		LWLockAcquire(stats_lock, LW_EXCLUSIVE);

		/*
		 * Use the lock in the first wait event to protect the reset timestamp
		 * as well.
		 */
		if (i == 0)
			pgStatLocal.shmem->wait_event.stats.stat_reset_timestamp = ts;

		memset(counters, 0, sizeof(*counters));
		LWLockRelease(stats_lock);
	}
}

void
pgstat_wait_event_snapshot_cb(void)
{
	for (int i = 0; i < NB_WAITCLASSTABLE_SIZE; i++)
	{
		LWLock	   *stats_lock = &pgStatLocal.shmem->wait_event.lock;
		PgStat_Counter *sh_counters = &pgStatLocal.shmem->wait_event.stats.counts[i];
		PgStat_Counter *counters_snap = &pgStatLocal.snapshot.wait_event.counts[i];

		LWLockAcquire(stats_lock, LW_SHARED);

		/*
		 * Use the lock in the first wait event to protect the reset timestamp
		 * as well.
		 */
		if (i == 0)
			pgStatLocal.snapshot.wait_event.stat_reset_timestamp =
				pgStatLocal.shmem->wait_event.stats.stat_reset_timestamp;

		/* using struct assignment due to better type safety */
		*counters_snap = *sh_counters;
		LWLockRelease(stats_lock);
	}
}

/*
 * Check if there any wait event stats waiting for flush.
 */
bool
pgstat_wait_event_have_pending_cb(void)
{
	return have_wait_event_stats;
}
