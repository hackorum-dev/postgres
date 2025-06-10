/* -------------------------------------------------------------------------
 *
 * pgstat_multixact.c
 *	  Implementation of multixact statistics.
 *
 * This file contains the implementation of multixact statistics. It is kept
 * separate from pgstat.c to enforce the line between the statistics access /
 * storage implementation and the details about individual types of
 * statistics.
 *
 * Copyright (c) 2001-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_multixact.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"
#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"

static bool have_multixact_stats = false;

/*
 * pgstat_fetch_stat_multixact()
 *
 * Support function for the SQL-callable pgstat* functions. Returns
 * a pointer to the multixact statistics struct.
 */
PgStat_MultiXactStats *
pgstat_fetch_stat_multixact(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_MULTIXACT);

	return &pgStatLocal.snapshot.multixact;
}

bool
pgstat_multixact_have_pending_cb(void)
{
	return have_multixact_stats;
}

void
pgstat_update_multixact_stats(uint32 nmembers)
{
	PgStat_MultiXactStats * local_stats;
	TimestampTz now;

	now = GetCurrentTimestamp();
	local_stats = &pgStatLocal.snapshot.multixact;
	local_stats->num_members_used = nmembers;
	local_stats->stat_update_timestamp = now;
	have_multixact_stats = true;
}

/*
 * Report multixact statistics. If nowait is true, then this function
 * will return if the lock is currently being held.
 *
 * This function returns true if the lock could not be acquired. Otherwise, false.
 */
bool
pgstat_flush_multixact_cb(bool nowait)
{
	PgStatShared_MultiXact	*stats_shmem;
	LWLock					*shared_lock;

	stats_shmem = &pgStatLocal.shmem->multixact;
	shared_lock = &pgStatLocal.shmem->multixact.lock;

	// Since this statistic is a gauge, we have to check timestamps
	// of the shared statistic and the local statistic. If ours is
	// larger, then we can overwrite the shared statistic.
	if (!nowait)
		LWLockAcquire(shared_lock, LW_EXCLUSIVE);
	else if (!LWLockConditionalAcquire(shared_lock, LW_EXCLUSIVE))
		return true;

	if (pgStatLocal.snapshot.multixact.stat_update_timestamp <= stats_shmem->stats.stat_update_timestamp)
	{
		// Return if our stats are <= the latest update.
		LWLockRelease(shared_lock);
		have_multixact_stats = false;
		return false;
	}

	// Update multixact member usage and latest timestamp.
	stats_shmem->stats.num_members_used = pgStatLocal.snapshot.multixact.num_members_used;
	stats_shmem->stats.stat_update_timestamp = pgStatLocal.snapshot.multixact.stat_update_timestamp;
	have_multixact_stats = false;

	LWLockRelease(shared_lock);
	return false;
}

void
pgstat_multixact_init_shmem_cb(void *stats)
{
	PgStatShared_MultiXact *stats_shmem;

	stats_shmem = (PgStatShared_MultiXact *) stats;
	LWLockInitialize(&stats_shmem->lock, LWTRANCHE_PGSTATS_DATA);
}

void
pgstat_multixact_snapshot_cb(void)
{
	PgStat_MultiXactStats *local_snapshot;
	PgStatShared_MultiXact *stats_shmem;

	local_snapshot = &pgStatLocal.snapshot.multixact;
	stats_shmem = &pgStatLocal.shmem->multixact;

	LWLockAcquire(&stats_shmem->lock, LW_SHARED);
	if (local_snapshot->stat_update_timestamp < stats_shmem->stats.stat_update_timestamp)
	{
		local_snapshot->stat_update_timestamp = stats_shmem->stats.stat_update_timestamp;
		local_snapshot->num_members_used = stats_shmem->stats.num_members_used;
	}
	LWLockRelease(&stats_shmem->lock);
}

void
pgstat_multixact_reset_all_cb(TimestampTz ts)
{
	PgStatShared_MultiXact *stats_shmem;

	stats_shmem = &pgStatLocal.shmem->multixact;
	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	memset(&stats_shmem->stats, 0, sizeof(stats_shmem->stats));
	LWLockRelease(&stats_shmem->lock);
}
