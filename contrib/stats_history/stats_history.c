/*-------------------------------------------------------------------------
 *
 * stats_history.c
 *		history of statistics about past runs of maintenance operations
 *
 * Information about runs of important maintenance operations (like vacuum
 * or checkpoint) are collected using hooks, and stored in a circular queue
 * with fixed size. After the queue gets full, old entries are overwritten.
 *
 * Copyright (c) 2008-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/stats_history/stats_history.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/objectaccess.h"
#include "commands/vacuum.h"
#include "funcapi.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/pg_lsn.h"
#include "utils/timestamp.h"


PG_MODULE_MAGIC;

/* Location of permanent stats file (valid when database is shut down) */
#define STATS_HISTORY_DUMP_FILE	PGSTAT_STAT_PERMANENT_DIRECTORY "/stats_history.stat"

/* Magic number identifying the stats file format (random value) */
static const uint32 STATS_HISTORY_FILE_HEADER = 0x6c309461;

/* PostgreSQL major version number, changes in which invalidate all entries */
static const uint32 STATS_PG_MAJOR_VERSION = PG_VERSION_NUM / 100;

/* Type of entry added to the queue. */
typedef enum EntryType
{
	ENTRY_VACUUM,
	ENTRY_CHECKPOINT
} EntryType;

typedef struct Entry
{
	EntryType	type;
	int32		len;	/* includes the Entry header, no alignment */
} Entry;

/* Information about one vacuum run, fields match the vacuum_log_hook_type. */
typedef struct VacuumEntry
{
	Entry		hdr;

	Oid			dbid;
	Oid			relid;

	TimestampTz	start_time;
	TimestampTz	end_time;

	bool		is_autovacuum;
	bool		is_aggressive;
	bool		is_wraparound;
	bool		index_cleanup;

	int64		pages_removed;
	int64		pages_remain;
	int64		pages_scanned;
	int64		pages_frozen;
	int64		pages_missed_dead;
	int64		pages_new_visible;
	int64		pages_new_frozen;
	int64		pages_new_visible_frozen;

	int64		tuples_removed;
	int64		tuples_remain;
	int64		tuples_not_removable;
	int64		tuples_frozen;
	int64		tuples_missed_dead;

	int32		removable_cutoff;
	int32		relfrozenxid_advance;
	int32		relminmxid_advance;

	int64		index_scans;
	int32		index_count;
	int64		index_pages;
	int64		index_pages_newly_deleted;
	int64		index_pages_deleted;
	int64		index_pages_free;

	double		io_read_ms;
	double		io_write_ms;

	int64		wal_records;
	int64		wal_fpis;
	int64		wal_bytes;

	double		cpu_user;
	double		cpu_system;
	double		cpu_elapsed;
} VacuumEntry;

/* Information about one checkpoint, fields match the checkpoint_log_hook_type. */
typedef struct CheckpointEntry
{
	Entry		hdr;

	TimestampTz	start_time;
	TimestampTz	end_time;

	bool		is_shutdown;
	bool		is_end_of_recovery;
	bool		is_immediate;
	bool		is_force;
	bool		is_wait;
	bool		is_wal;
	bool		is_time;
	bool		is_flush_all;

	int			buffers_written;
	int			slru_written;
	int			segs_added;
	int			segs_removed;
	int			segs_recycled;

	long		write_ms;
	long		sync_ms;
	long		total_ms;

	int			sync_files;
	long		sync_longest_ms;
	long		sync_average_ms;

	double		distance_prev;
	double		distance_est;

	XLogRecPtr	lsn;
	XLogRecPtr	redo_lsn;
} CheckpointEntry;

/*
 * Global shared state - circular queue of entries.
 *
 * queueSize - Size of the queue, in bytes.
 *
 * queueStart/queueNext - Byte positions of the first used/free byte. The
 * values are growing, i.e. not subject to modulo size.
 *
 * The size and positions are always multiples of MAXALIGN(Entry size). This
 * makes the queue management easier - it means the in-queue entries are always
 * properly aligned and can be referenced directly, and the header can never
 * be "split" at the end of the queue. It also makes it easier to calculate
 * free space, because we can simply subtract the start/end offsets (we don't
 * need to worry about not splitting the header, etc.)
 *
 * Entries are evicted from the oldest, using the end timestamp.
 */
typedef struct StatsHistorySharedState
{
	LWLock	   *lock;			/* protects queue modification */
	int64		queueStart;		/* first used byte */
	int64		queueNext;		/* first free byte */
	int64		queueSize;		/* queue capacity */
	char		queue[FLEXIBLE_ARRAY_MEMBER];	/* queue data */
} StatsHistorySharedState;

/* size of the shared space (with history etc.) */
static int		statsHistorySizeMB;

/* Saved hook values in case of unload */
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static vacuum_log_hook_type prev_vacuum_log_hook = NULL;
static checkpoint_log_hook_type prev_checkpoint_log_hook = NULL;
static object_access_hook_type prev_object_access_hook = NULL;

/* Links to shared memory state */
static StatsHistorySharedState *sharedState = NULL;

/*---- GUC variables ----*/
static bool vacuumStatsSave = true;

/*---- Function declarations ----*/

PG_FUNCTION_INFO_V1(stats_history_reset);

PG_FUNCTION_INFO_V1(vacuum_history_1_0);
PG_FUNCTION_INFO_V1(checkpoint_history_1_0);

static void vacuum_history_internal(FunctionCallInfo fcinfo);
static void checkpoint_history_internal(FunctionCallInfo fcinfo);

static void stats_history_shmem_request(void);
static void stats_history_shmem_startup(void);
static void stats_history_shmem_shutdown(int code, Datum arg);
static Size stats_history_memsize(void);

/* serialize and deserialize data into file */
static void stats_history_save_history(void);
static void stats_history_load_history(void);

/* queue management */
static int64 queue_free_space(void);
static int64 queue_next_entry(int64 index, Size len);
static Entry *queue_read_entry(int64 index);
static void queue_add_entry(Entry *entry);
static int32 queue_entry_len(int32 len);

/* hooks to collect information about maintenance operations */

static void vacuum_history_log_hook(TimestampTz starttime, TimestampTz endtime,
									Oid relid,
									bool is_autovacuum, bool is_aggressive,
									bool is_wraparound, bool index_cleanup,
									int64 pages_removed, int64 pages_remain,
									int64 pages_scanned, int64 pages_frozen,
									int64 pages_missed_dead,
									int64 pages_new_visible, int64 pages_new_frozen,
									int64 pages_new_visible_frozen,
									int64 tuples_removed, int64 tuples_remain,
									int64 tuples_not_removable, int64 tuples_frozen,
									int64 tuples_missed_dead,
									int32 removable_cutoff, int32 relfrozenxid_advance,
									int32 relminmxid_advance,
									int64 index_scans, int32 index_count, int64 index_pages,
									int64 index_pages_newly_deleted,
									int64 index_pages_deleted, int64 index_pages_free,
									double io_read_ms, double io_write_ms,
									int64 wal_records, int64 wal_fpis, int64 wal_bytes,
									double cpu_user, double cpu_system, double cpu_elapsed);

static void checkpoint_history_log_hook(
						TimestampTz start_time,
						TimestampTz end_time,
						bool is_shutdown,
						bool is_end_of_recovery,
						bool is_immediate,
						bool is_force,
						bool is_wait,
						bool is_wal,
						bool is_time,
						bool is_flush_all,
						int buffers_written,
						int slru_written,
						int segs_added,
						int segs_removed,
						int segs_recycled,
						long write_ms,
						long sync_ms,
						long total_ms,
						int sync_files,
						long sync_longest_ms,
						long sync_average_ms,
						double distance_prev,
						double distance_est,
						XLogRecPtr lsn,
						XLogRecPtr redo_lsn);

/* handle dropping objects (relations, databases) */
static void stats_history_object_access_hook(ObjectAccessType access,
											Oid classId,
											Oid objectId,
											int subId,
											void *arg);

static void
AssertCheckQueue(void)
{
#ifdef USE_ASSERT_CHECKING
	Assert(LWLockHeldByMe(sharedState->lock));

	/* size and both positions are multiple of entry header */
	Assert(sharedState->queueSize % sizeof(Entry) == 0);
	Assert(sharedState->queueStart % sizeof(Entry) == 0);
	Assert(sharedState->queueNext % sizeof(Entry) == 0);

	Assert(sharedState->queueStart <= sharedState->queueNext);
	Assert(sharedState->queueNext - sharedState->queueStart <= sharedState->queueSize);
#endif
}

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/*
	 * In order to create our shared memory area, we have to be loaded via
	 * shared_preload_libraries.  If not, fall out without hooking into any of
	 * the main system.  (We don't throw error here because it seems useful to
	 * allow the pg_stat_statements functions to be created even when the
	 * module isn't active.  The functions must protect themselves against
	 * being called then, however.)
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	/*
	 * Define (or redefine) custom GUC variables.
	 */
	DefineCustomIntVariable("stats_history.size",
							"Sets the amount of memory available for past events.",
							NULL,
							&statsHistorySizeMB,
							1,
							1,
							128,
							PGC_POSTMASTER,
							GUC_UNIT_MB,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("stats_history.save",
							 "Save stats_history statistics across server shutdowns.",
							 NULL,
							 &vacuumStatsSave,
							 true,
							 PGC_SIGHUP,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("stats_history");

	/*
	 * Install hooks.
	 */
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = stats_history_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = stats_history_shmem_startup;

	prev_vacuum_log_hook = vacuum_log_hook;
	vacuum_log_hook = vacuum_history_log_hook;

	prev_checkpoint_log_hook = checkpoint_log_hook;
	checkpoint_log_hook = checkpoint_history_log_hook;

	prev_object_access_hook = object_access_hook;
	object_access_hook = stats_history_object_access_hook;
}

/*
 * Estimate shared memory space needed.
 */
static Size
stats_history_memsize(void)
{
	Size		size;

	size = MAXALIGN(offsetof(StatsHistorySharedState, queue) +
					(statsHistorySizeMB * 1024L * 1024L));

	return size;
}

/*
 * shmem_request hook: request additional shared resources.  We'll allocate or
 * attach to the shared resources in stats_history_shmem_startup().
 */
static void
stats_history_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(stats_history_memsize());
	RequestNamedLWLockTranche("stats_history", 1);
}

/*
 * shmem_startup hook: allocate or attach to shared memory, then load any
 * pre-existing statistics from file.
 */
static void
stats_history_shmem_startup(void)
{
	bool	found;

	Size	queueSize = (statsHistorySizeMB * 1024L * 1024L);
	Size	stateSize = offsetof(StatsHistorySharedState, queue) + queueSize;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* reset in case this is a restart within the postmaster */
	sharedState = NULL;

	/*
	 * Create or attach to the shared memory state, including hash table
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	sharedState = ShmemInitStruct("stats_history",
								  stateSize,
								  &found);

	if (!found)
	{
		/* First time through ... */
		sharedState->lock = &(GetNamedLWLockTranche("stats_history"))->lock;

		sharedState->queueStart = 0;
		sharedState->queueNext = 0;
		sharedState->queueSize = queueSize;
	}

	LWLockRelease(AddinShmemInitLock);

	/*
	 * If we're in the postmaster (or a standalone backend...), set up a shmem
	 * exit hook to dump the statistics to disk.
	 */
	if (!IsUnderPostmaster)
		on_shmem_exit(stats_history_shmem_shutdown, (Datum) 0);

	/*
	 * Done if some other process already completed our initialization.
	 */
	if (found)
		return;

	/*
	 * If we were told not to load old statistics, we're done.  (Note we do
	 * not try to unlink any old dump file in this case.  This seems a bit
	 * questionable as we won't remove entries for dropped objects.)
	 */
	if (!vacuumStatsSave)
		return;

	stats_history_load_history();
}

static void
stats_history_save_history(void)
{
	FILE	   *file;
	int64		index;

	LWLockAcquire(sharedState->lock, LW_SHARED);

	file = AllocateFile(STATS_HISTORY_DUMP_FILE ".tmp", PG_BINARY_W);
	if (file == NULL)
		goto error;

	if (fwrite(&STATS_HISTORY_FILE_HEADER, sizeof(uint32), 1, file) != 1)
		goto error;

	if (fwrite(&STATS_PG_MAJOR_VERSION, sizeof(uint32), 1, file) != 1)
		goto error;

	index = sharedState->queueStart;
	while (index < sharedState->queueNext)
	{
		Entry  *entry = queue_read_entry(index);

		if (fwrite(entry, entry->len, 1, file) != 1)
			goto error;

		index = queue_next_entry(index, entry->len);
		pfree(entry);
	}

	if (FreeFile(file))
	{
		file = NULL;
		goto error;
	}

	/*
	 * Rename file into place, so we atomically replace any old one.
	 */
	(void) durable_rename(STATS_HISTORY_DUMP_FILE ".tmp", STATS_HISTORY_DUMP_FILE, LOG);

	LWLockRelease(sharedState->lock);

	return;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					STATS_HISTORY_DUMP_FILE ".tmp")));
	if (file)
		FreeFile(file);
	unlink(STATS_HISTORY_DUMP_FILE ".tmp");

	LWLockRelease(sharedState->lock);
}

static void
stats_history_load_history(void)
{
	FILE	   *file = NULL;
	uint32		header;
	int32		pgver;

	LWLockAcquire(sharedState->lock, LW_EXCLUSIVE);

	/*
	 * Attempt to load old statistics from the dump file.
	 */
	file = AllocateFile(STATS_HISTORY_DUMP_FILE, PG_BINARY_R);
	if (file == NULL)
	{
		if (errno != ENOENT)
			goto read_error;
		/* No existing persisted stats file, so we're done */
		LWLockRelease(sharedState->lock);
		return;
	}

	if (fread(&header, sizeof(uint32), 1, file) != 1 ||
		fread(&pgver, sizeof(uint32), 1, file) != 1)
		goto read_error;

	if (header != STATS_HISTORY_FILE_HEADER ||
		pgver != STATS_PG_MAJOR_VERSION)
		goto data_error;

	while (true)
	{
		Entry	header;
		Entry  *entry;
		char   *ptr;

		if (fread(&header, sizeof(Entry), 1, file) != 1)
			goto read_error;

		ptr = palloc(header.len);
		entry = (Entry *) ptr;

		memcpy(ptr, &header, sizeof(Entry));
		ptr += sizeof(Entry);

		/* XXX this can happen also when we reach end of file */
		if (fread(ptr, entry->len - sizeof(Entry), 1, file) != 1)
			goto read_error;

		/* no lock, there should be no concurrently added entries */
		queue_add_entry(entry);
		pfree(entry);
	}

	FreeFile(file);

	/*
	 * Remove the persisted stats file so it's not included in
	 * backups/replication standbys, etc.  A new file will be written on next
	 * shutdown.
	 */
	unlink(STATS_HISTORY_DUMP_FILE);

	LWLockRelease(sharedState->lock);

	return;

read_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not read file \"%s\": %m",
					STATS_HISTORY_DUMP_FILE)));
	goto fail;
data_error:
	ereport(LOG,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("ignoring invalid data in file \"%s\"",
					STATS_HISTORY_DUMP_FILE)));
	goto fail;
fail:
	if (file)
		FreeFile(file);

	/* If possible, throw away the bogus file; ignore any error */
	unlink(STATS_HISTORY_DUMP_FILE);

	LWLockRelease(sharedState->lock);
}

/*
 * shmem_shutdown hook: Dump statistics into file.
 *
 * Note: we don't bother with acquiring lock, because there should be no
 * other processes running when this is called.
 */
static void
stats_history_shmem_shutdown(int code, Datum arg)
{
	/* Don't try to dump during a crash. */
	if (code)
		return;

	if (!sharedState)
		return;

	if (!vacuumStatsSave)
		return;

	stats_history_save_history();
}

/*
 * Reset vacuum history.
 */
Datum
stats_history_reset(PG_FUNCTION_ARGS)
{
	LWLockAcquire(sharedState->lock, LW_SHARED);
	sharedState->queueNext = 0;
	LWLockRelease(sharedState->lock);

	PG_RETURN_VOID();
}

/* handle DROP TABLE and DROP DATABASE events - remove the entries */
static void
stats_history_object_access_hook(ObjectAccessType access,
								Oid classId,
								Oid objectId,
								int subId,
								void *arg)
{
	int64	index,
			next;
	Oid		relOid,
			dbOid;

	/* only care about DROP actions */
	if (access != OAT_DROP)
		return;

	/* only care about tables and dabases */
	if (classId == RelationRelationId)
	{
		dbOid = MyDatabaseId;
		relOid = objectId;
	}
	else if (classId == DatabaseRelationId)
	{
		dbOid = objectId;
		relOid = InvalidOid;
	}
	else
		return;

	LWLockAcquire(sharedState->lock, LW_EXCLUSIVE);

	AssertCheckQueue();

	index = sharedState->queueStart;
	next = sharedState->queueNext;

	/*
	 * Pretend the queue is empty - we'll only compact the entries, the
	 * amount of data can never expand.
	 */
	sharedState->queueNext = sharedState->queueStart;

	while (index < next)
	{
		Entry	    *hdr = queue_read_entry(index);

		/* not a filtering matters only for vacuum entries */
		if (hdr->type == ENTRY_VACUUM)
		{
			VacuumEntry *entry = (VacuumEntry *) hdr;

			/* keep the entry if it doesn't match the object we're dropping */
			if ((entry->dbid != dbOid) ||
				(relOid != InvalidOid && entry->relid != relOid))
			{
				queue_add_entry(hdr);
			}
		}

		/* advance to index */
		index = queue_next_entry(index, hdr->len);
		pfree(hdr);
	}

	LWLockRelease(sharedState->lock);
}

/* vacuum */

#define	VACUUM_HISTORY_COLS_V1_0	38
#define	VACUUM_HISTORY_COLS			38

Datum
vacuum_history_1_0(PG_FUNCTION_ARGS)
{
	vacuum_history_internal(fcinfo);

	return (Datum) 0;
}

static void
vacuum_history_internal(FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			userid = GetUserId();
	int64		index;

	/*
	 * Superusers or roles with the privileges of pg_read_all_stats members
	 * are allowed
	 */
	if (!has_privs_of_role(userid, ROLE_PG_READ_ALL_STATS))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("vacuum_history privilege error")));

	/* hash table must exist already */
	if (!sharedState)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("vacuum_history must be loaded via \"shared_preload_libraries\"")));

	InitMaterializedSRF(fcinfo, 0);

	LWLockAcquire(sharedState->lock, LW_SHARED);

	index = sharedState->queueStart;
	while (index < sharedState->queueNext)
	{
		Entry	    *hdr = queue_read_entry(index);

		/* not a vacuum entry */
		if (hdr->type == ENTRY_VACUUM)
		{
			int			i = 0;
			Datum		values[VACUUM_HISTORY_COLS];
			bool		nulls[VACUUM_HISTORY_COLS];

			VacuumEntry *entry = (VacuumEntry *) hdr;

			/* only show data for my database (so that we can translate relid) */
			if (entry->dbid != MyDatabaseId)
				continue;

			memset(values, 0, sizeof(values));
			memset(nulls, 0, sizeof(nulls));

			values[i++] = TimestampTzGetDatum(entry->start_time);
			values[i++] = TimestampTzGetDatum(entry->end_time);

			values[i++] = ObjectIdGetDatum(entry->dbid);
			values[i++] = ObjectIdGetDatum(entry->relid);

			values[i++] = BoolGetDatum(entry->is_autovacuum);
			values[i++] = BoolGetDatum(entry->is_aggressive);
			values[i++] = BoolGetDatum(entry->is_wraparound);
			values[i++] = BoolGetDatum(entry->index_cleanup);

			values[i++] = Int64GetDatum(entry->pages_removed);
			values[i++] = Int64GetDatum(entry->pages_remain);
			values[i++] = Int64GetDatum(entry->pages_scanned);
			values[i++] = Int64GetDatum(entry->pages_frozen);
			values[i++] = Int64GetDatum(entry->pages_missed_dead);
			values[i++] = Int64GetDatum(entry->pages_new_visible);
			values[i++] = Int64GetDatum(entry->pages_new_frozen);
			values[i++] = Int64GetDatum(entry->pages_new_visible_frozen);

			values[i++] = Int64GetDatum(entry->tuples_removed);
			values[i++] = Int64GetDatum(entry->tuples_remain);
			values[i++] = Int64GetDatum(entry->tuples_not_removable);
			values[i++] = Int64GetDatum(entry->tuples_frozen);
			values[i++] = Int64GetDatum(entry->tuples_missed_dead);

			values[i++] = Int32GetDatum(entry->removable_cutoff);
			values[i++] = Int32GetDatum(entry->relfrozenxid_advance);
			values[i++] = Int32GetDatum(entry->relminmxid_advance);

			values[i++] = Int64GetDatum(entry->index_scans);
			values[i++] = Int32GetDatum(entry->index_count);
			values[i++] = Int64GetDatum(entry->index_pages);
			values[i++] = Int64GetDatum(entry->index_pages_newly_deleted);
			values[i++] = Int64GetDatum(entry->index_pages_deleted);
			values[i++] = Int64GetDatum(entry->index_pages_free);

			values[i++] = Float4GetDatum(entry->io_read_ms);
			values[i++] = Float4GetDatum(entry->io_write_ms);

			values[i++] = Int64GetDatum(entry->wal_records);
			values[i++] = Int64GetDatum(entry->wal_fpis);
			values[i++] = Int64GetDatum(entry->wal_bytes);

			values[i++] = Float8GetDatum(entry->cpu_user);
			values[i++] = Float8GetDatum(entry->cpu_system);
			values[i++] = Float8GetDatum(entry->cpu_elapsed);

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}

		/* advance index */
		index = queue_next_entry(index, hdr->len);
		pfree(hdr);
	}

	LWLockRelease(sharedState->lock);
}

static void
vacuum_history_log_hook(TimestampTz starttime, TimestampTz endtime, Oid relid,
						bool is_autovacuum, bool is_aggressive, bool is_wraparound,
						bool index_cleanup,
						int64 pages_removed, int64 pages_remain, int64 pages_scanned,
						int64 pages_frozen, int64 pages_missed_dead,
						int64 pages_new_visible, int64 pages_new_frozen, int64 pages_new_visible_frozen,
						int64 tuples_removed, int64 tuples_remain, int64 tuples_not_removable,
						int64 tuples_frozen, int64 tuples_missed_dead,
						int32 removable_cutoff, int32 relfrozenxid_advance, int32 relminmxid_advance,
						int64 index_scans, int32 index_count, int64 index_pages,
						int64 index_pages_newly_deleted, int64 index_pages_deleted, int64 index_pages_free,
						double io_read_ms, double io_write_ms,
						int64 wal_records, int64 wal_fpis, int64 wal_bytes,
						double cpu_user, double cpu_system, double cpu_elapsed)
{
	VacuumEntry		entry;

	if (prev_vacuum_log_hook)
		prev_vacuum_log_hook(starttime, endtime, relid,
							 is_autovacuum, is_aggressive, is_wraparound,
							 index_cleanup,
							 pages_removed, pages_remain, pages_scanned,
							 pages_frozen, pages_missed_dead,
							 pages_new_visible, pages_new_frozen, pages_new_visible_frozen,
							 tuples_removed, tuples_remain, tuples_not_removable,
							 tuples_frozen, tuples_missed_dead,
							 removable_cutoff, relfrozenxid_advance, relminmxid_advance,
							 index_scans, index_count, index_pages, index_pages_newly_deleted,
							 index_pages_deleted, index_pages_free,
							 io_read_ms, io_write_ms,
							 wal_records, wal_fpis, wal_bytes,
							 cpu_user, cpu_system, cpu_elapsed);

	entry.hdr.type = ENTRY_VACUUM;
	entry.hdr.len = sizeof(VacuumEntry);

	entry.start_time = starttime;
	entry.end_time = endtime;
	entry.dbid = MyDatabaseId;
	entry.relid = relid;
	entry.is_autovacuum = is_autovacuum;
	entry.is_aggressive = is_aggressive;
	entry.is_wraparound = is_wraparound;
	entry.index_cleanup = index_cleanup;
	entry.index_scans = index_scans;
	entry.pages_removed = pages_removed;
	entry.pages_remain = pages_remain;
	entry.pages_scanned = pages_scanned;
	entry.pages_frozen = pages_frozen;
	entry.pages_missed_dead = pages_missed_dead;
	entry.pages_new_visible = pages_new_visible;
	entry.pages_new_frozen = pages_new_frozen;
	entry.pages_new_visible_frozen = pages_new_visible_frozen;
	entry.tuples_removed = tuples_removed;
	entry.tuples_remain = tuples_remain;
	entry.tuples_not_removable = tuples_not_removable;
	entry.tuples_frozen = tuples_frozen;
	entry.tuples_missed_dead = tuples_missed_dead;
	entry.removable_cutoff = removable_cutoff;
	entry.relfrozenxid_advance = relfrozenxid_advance;
	entry.relminmxid_advance = relminmxid_advance;
	entry.index_count = index_count;
	entry.index_pages = index_pages;
	entry.index_pages_newly_deleted = index_pages_newly_deleted;
	entry.index_pages_deleted = index_pages_deleted;
	entry.index_pages_free = index_pages_free;
	entry.io_read_ms = io_read_ms;
	entry.io_write_ms = io_write_ms;
	entry.wal_records = wal_records;
	entry.wal_fpis = wal_fpis;
	entry.wal_bytes = wal_bytes;
	entry.cpu_user = cpu_user;
	entry.cpu_system = cpu_system;
	entry.cpu_elapsed = cpu_elapsed;

	LWLockAcquire(sharedState->lock, LW_EXCLUSIVE);

	queue_add_entry((Entry *) &entry);

	LWLockRelease(sharedState->lock);
}

/* checkpoint */

#define	CHECKPOINT_HISTORY_COLS_V1_0	25
#define	CHECKPOINT_HISTORY_COLS			25

Datum
checkpoint_history_1_0(PG_FUNCTION_ARGS)
{
	checkpoint_history_internal(fcinfo);

	return (Datum) 0;
}

static void
checkpoint_history_internal(FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			userid = GetUserId();
	int64		index;

	/*
	 * Superusers or roles with the privileges of pg_read_all_stats members
	 * are allowed
	 */
	if (!has_privs_of_role(userid, ROLE_PG_READ_ALL_STATS))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("vacuum_history privilege error")));

	/* hash table must exist already */
	if (!sharedState)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("vacuum_history must be loaded via \"shared_preload_libraries\"")));

	InitMaterializedSRF(fcinfo, 0);

	LWLockAcquire(sharedState->lock, LW_SHARED);

	index = sharedState->queueStart;
	while (index < sharedState->queueNext)
	{
		Entry	    *hdr = queue_read_entry(index);

		/* not a checkpoint entry */
		if (hdr->type == ENTRY_CHECKPOINT)
		{
			int			i = 0;
			Datum		values[CHECKPOINT_HISTORY_COLS];
			bool		nulls[CHECKPOINT_HISTORY_COLS];

			CheckpointEntry *entry = (CheckpointEntry *) hdr;

			memset(values, 0, sizeof(values));
			memset(nulls, 0, sizeof(nulls));

			values[i++] = TimestampTzGetDatum(entry->start_time);
			values[i++] = TimestampTzGetDatum(entry->end_time);

			values[i++] = BoolGetDatum(entry->is_shutdown);
			values[i++] = BoolGetDatum(entry->is_end_of_recovery);
			values[i++] = BoolGetDatum(entry->is_immediate);
			values[i++] = BoolGetDatum(entry->is_force);
			values[i++] = BoolGetDatum(entry->is_wait);
			values[i++] = BoolGetDatum(entry->is_wal);
			values[i++] = BoolGetDatum(entry->is_time);
			values[i++] = BoolGetDatum(entry->is_flush_all);

			values[i++] = Int32GetDatum(entry->buffers_written);
			values[i++] = Int32GetDatum(entry->slru_written);
			values[i++] = Int32GetDatum(entry->segs_added);
			values[i++] = Int32GetDatum(entry->segs_removed);
			values[i++] = Int32GetDatum(entry->segs_recycled);

			values[i++] = Int64GetDatum(entry->write_ms);
			values[i++] = Int64GetDatum(entry->sync_ms);
			values[i++] = Int64GetDatum(entry->total_ms);

			values[i++] = Int32GetDatum(entry->sync_files);

			values[i++] = Int64GetDatum(entry->sync_longest_ms);
			values[i++] = Int64GetDatum(entry->sync_average_ms);

			values[i++] = Float8GetDatum(entry->distance_prev);
			values[i++] = Float8GetDatum(entry->distance_est);

			values[i++] = LSNGetDatum(entry->lsn);
			values[i++] = LSNGetDatum(entry->redo_lsn);

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}

		/* advance to index */
		index = queue_next_entry(index, hdr->len);
		pfree(hdr);
	}

	LWLockRelease(sharedState->lock);
}

static void
checkpoint_history_log_hook(TimestampTz start_time, TimestampTz end_time,
							bool is_shutdown, bool is_end_of_recovery,
							bool is_immediate, bool is_force, bool is_wait,
							bool is_wal, bool is_time, bool is_flush_all,
							int buffers_written, int slru_written,
							int segs_added, int segs_removed, int segs_recycled,
							long write_ms, long sync_ms, long total_ms,
							int sync_files, long sync_longest_ms, long sync_average_ms,
							double distance_prev, double distance_est,
							XLogRecPtr lsn, XLogRecPtr redo_lsn)
{
	CheckpointEntry		entry;

	if (prev_vacuum_log_hook)
		prev_checkpoint_log_hook(start_time, end_time,
							is_shutdown, is_end_of_recovery,
							is_immediate, is_force, is_wait,
							is_wal, is_time, is_flush_all,
							buffers_written, slru_written,
							segs_added, segs_removed, segs_recycled,
							write_ms, sync_ms, total_ms,
							sync_files, sync_longest_ms, sync_average_ms,
							distance_prev, distance_est,
							lsn, redo_lsn);

	entry.hdr.type = ENTRY_CHECKPOINT;
	entry.hdr.len = sizeof(CheckpointEntry);

	entry.start_time = start_time;
	entry.end_time = end_time;

	entry.is_shutdown = is_shutdown;
	entry.is_end_of_recovery = is_end_of_recovery;
	entry.is_immediate = is_immediate;
	entry.is_force = is_force;
	entry.is_wait = is_wait;
	entry.is_wal = is_wal;
	entry.is_time = is_time;
	entry.is_flush_all = is_flush_all;

	entry.buffers_written = buffers_written;
	entry.slru_written = slru_written;
	entry.segs_added = segs_added;
	entry.segs_removed = segs_removed;
	entry.segs_recycled = segs_recycled;

	entry.write_ms = write_ms;
	entry.sync_ms = sync_ms;
	entry.total_ms = total_ms;

	entry.sync_files = sync_files;
	entry.sync_longest_ms = sync_longest_ms;
	entry.sync_average_ms = sync_average_ms;

	entry.distance_prev = distance_prev;
	entry.distance_est = distance_est;
	entry.lsn = lsn;
	entry.redo_lsn = redo_lsn;

	LWLockAcquire(sharedState->lock, LW_EXCLUSIVE);

	queue_add_entry((Entry *) &entry);

	LWLockRelease(sharedState->lock);
}

/* return the position of next extry after the one starting at index */
static int64
queue_next_entry(int64 index, Size len)
{
	AssertCheckQueue();

	/* calculate the next entry index */
	return (index + queue_entry_len(len));
}

/* how much free space is there for new entries */
static int64
queue_free_space(void)
{
	int64	used = (sharedState->queueNext - sharedState->queueStart);
	int64	free = (sharedState->queueSize - used);

	AssertCheckQueue();

	return free;
}

/* XXX assumes the queue is properly locked */
static Entry *
queue_read_entry(int64 index)
{
	Entry  *tmp;
	char   *ptr;
	Entry  *entry;
	int64	nbytes;

	AssertCheckQueue();

	tmp = (Entry *) &sharedState->queue[index % sharedState->queueSize];

	nbytes = tmp->len;
	ptr = palloc(tmp->len);
	entry = (Entry *) ptr;

	while (nbytes > 0)
	{
		int64	next = (index % sharedState->queueSize);
		int64	nspace = (sharedState->queueSize - next);
		int64	ncopy = Min(nspace, nbytes);

		memcpy(ptr, &sharedState->queue[next], ncopy);

		index += ncopy;
		nbytes -= ncopy;
		ptr += ncopy;
	}

	return entry;
}

/* round the length to a multiple of header size (with alignment) */
static int32
queue_entry_len(int32 len)
{
	Size	hlen = MAXALIGN(sizeof(Entry));

	return ((len + (hlen - 1)) / hlen) * hlen;
}

/* XXX assumes the queue is locked */
static void
queue_add_entry(Entry *entry)
{
	char   *ptr = (char *) entry;
	int		nbytes = entry->len;
	int		next = sharedState->queueNext;
	int		len = queue_entry_len(entry->len);

	AssertCheckQueue();

	Assert(LWLockHeldByMeInMode(sharedState->lock, LW_EXCLUSIVE));

	/* make sure we have space for the new entry in the queue */
	while (queue_free_space() < len)
	{
		int64	start;
		Entry  *tmp;

		start = sharedState->queueStart % sharedState->queueSize;
		tmp = (Entry *) &sharedState->queue[start];

		/* this ensures we never wrap the header */
		sharedState->queueStart
			= queue_next_entry(sharedState->queueStart, tmp->len);
	}

	/* copy the data, maybe wrap around */
	while (nbytes > 0)
	{
		int64	nfree,
				ncopy;

		next = (next % sharedState->queueSize);
		nfree = (sharedState->queueSize - next);
		ncopy = Min(nfree, nbytes);

		memcpy(&sharedState->queue[next], ptr, ncopy);

		next += ncopy;
		nbytes -= ncopy;
		ptr += ncopy;
	}

	sharedState->queueNext += len;
}
