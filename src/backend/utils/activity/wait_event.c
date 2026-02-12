/* ----------
 * wait_event.c
 *	  Wait event reporting infrastructure.
 *
 * Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/wait_event.c
 *
 * NOTES
 *
 * To make pgstat_report_wait_start() and pgstat_report_wait_end() as
 * lightweight as possible, they do not check if shared memory (MyProc
 * specifically, where the wait event is stored) is already available. Instead
 * we initially set my_wait_event_info to a process local variable, which then
 * is redirected to shared memory using pgstat_set_wait_event_storage(). For
 * the same reason pgstat_track_activities is not checked - the check adds
 * more work than it saves.
 *
 * ----------
 */
#include "postgres.h"

#include "port/atomics.h"
#include "storage/lmgr.h"		/* for GetLockNameFromTagType */
#include "storage/lwlock.h"		/* for GetLWLockIdentifier */
#include "storage/spin.h"
#include "utils/wait_event.h"


static const char *pgstat_get_wait_activity(WaitEventActivity w);
static const char *pgstat_get_wait_buffer(WaitEventBuffer w);
static const char *pgstat_get_wait_client(WaitEventClient w);
static const char *pgstat_get_wait_ipc(WaitEventIPC w);
static const char *pgstat_get_wait_timeout(WaitEventTimeout w);
static const char *pgstat_get_wait_io(WaitEventIO w);


static volatile pg_atomic_uint64 local_my_wait_event_info;
volatile pg_atomic_uint64 *my_wait_event_info = &local_my_wait_event_info;

#define WAIT_EVENT_CLASS_MASK	0xFF00000000000000
#define WAIT_EVENT_ID_MASK		0x0000FFFF00000000
#define WAIT_EVENT_ARG_MASK		0x00000000FFFFFFFF
/* just upper 24bits for decoding wait_event into name */
#define WAIT_EVENT_CLASSID_MASK (WAIT_EVENT_CLASS_MASK | WAIT_EVENT_ID_MASK)

/*
 * Hash tables for storing custom wait event ids and their names in
 * shared memory.
 *
 * WaitEventCustomHashByInfo is used to find the name from wait event
 * information.  Any backend can search it to find custom wait events.
 *
 * WaitEventCustomHashByName is used to find the wait event information from a
 * name.  It is used to ensure that no duplicated entries are registered.
 *
 * For simplicity, we use the same ID counter across types of custom events.
 * We could end that anytime the need arises.
 *
 * The size of the hash table is based on the assumption that
 * WAIT_EVENT_CUSTOM_HASH_INIT_SIZE is enough for most cases, and it seems
 * unlikely that the number of entries will reach
 * WAIT_EVENT_CUSTOM_HASH_MAX_SIZE.
 */
static HTAB *WaitEventCustomHashByInfo; /* find names from infos */
static HTAB *WaitEventCustomHashByName; /* find infos from names */

#define WAIT_EVENT_CUSTOM_HASH_INIT_SIZE	16
#define WAIT_EVENT_CUSTOM_HASH_MAX_SIZE	128

/* hash table entries */
typedef struct WaitEventCustomEntryByInfo
{
	uint64		wait_event_info;	/* hash key */
	char		wait_event_name[NAMEDATALEN];	/* custom wait event name */
} WaitEventCustomEntryByInfo;

typedef struct WaitEventCustomEntryByName
{
	char		wait_event_name[NAMEDATALEN];	/* hash key */
	uint64		wait_event_info;
} WaitEventCustomEntryByName;


/* dynamic allocation counter for custom wait events */
typedef struct WaitEventCustomCounterData
{
	int		nextId;			/* next ID to assign */
	slock_t		mutex;			/* protects the counter */
} WaitEventCustomCounterData;

/* pointer to the shared memory */
static WaitEventCustomCounterData *WaitEventCustomCounter;

/* first event ID of custom wait events */
#define WAIT_EVENT_CUSTOM_INITIAL_ID	1

static uint64 WaitEventCustomNew(uint64 classId, const char *wait_event_name);
static const char *GetWaitEventCustomIdentifier(uint64 wait_event_info);

/*
 *  Return the space for dynamic shared hash tables and dynamic allocation counter.
 */
Size
WaitEventCustomShmemSize(void)
{
	Size		sz;

	sz = MAXALIGN(sizeof(WaitEventCustomCounterData));
	sz = add_size(sz, hash_estimate_size(WAIT_EVENT_CUSTOM_HASH_MAX_SIZE,
										 sizeof(WaitEventCustomEntryByInfo)));
	sz = add_size(sz, hash_estimate_size(WAIT_EVENT_CUSTOM_HASH_MAX_SIZE,
										 sizeof(WaitEventCustomEntryByName)));
	return sz;
}

/*
 * Allocate shmem space for dynamic shared hash and dynamic allocation counter.
 */
void
WaitEventCustomShmemInit(void)
{
	bool		found;
	HASHCTL		info;

	WaitEventCustomCounter = (WaitEventCustomCounterData *)
		ShmemInitStruct("WaitEventCustomCounterData",
						sizeof(WaitEventCustomCounterData), &found);

	if (!found)
	{
		/* initialize the allocation counter and its spinlock. */
		WaitEventCustomCounter->nextId = WAIT_EVENT_CUSTOM_INITIAL_ID;
		SpinLockInit(&WaitEventCustomCounter->mutex);
	}

	/* initialize or attach the hash tables to store custom wait events */
	info.keysize = sizeof(uint64);
	info.entrysize = sizeof(WaitEventCustomEntryByInfo);
	WaitEventCustomHashByInfo =
		ShmemInitHash("WaitEventCustom hash by wait event information",
					  WAIT_EVENT_CUSTOM_HASH_INIT_SIZE,
					  WAIT_EVENT_CUSTOM_HASH_MAX_SIZE,
					  &info,
					  HASH_ELEM | HASH_BLOBS);

	/* key is a NULL-terminated string */
	info.keysize = sizeof(char[NAMEDATALEN]);
	info.entrysize = sizeof(WaitEventCustomEntryByName);
	WaitEventCustomHashByName =
		ShmemInitHash("WaitEventCustom hash by name",
					  WAIT_EVENT_CUSTOM_HASH_INIT_SIZE,
					  WAIT_EVENT_CUSTOM_HASH_MAX_SIZE,
					  &info,
					  HASH_ELEM | HASH_STRINGS);
}

/*
 * Allocate a new event ID and return the wait event info.
 *
 * If the wait event name is already defined, this does not allocate a new
 * entry; it returns the wait event information associated to the name.
 *
 * XXX: maybe we should return safe u64 struct here so that extension
 * writers will get clear error message about need to change the type
 */
uint64
WaitEventExtensionNew(const char *wait_event_name)
{
	return WaitEventCustomNew(PG_WAIT_EXTENSION, wait_event_name);
}

uint64
WaitEventInjectionPointNew(const char *wait_event_name)
{
	return WaitEventCustomNew(PG_WAIT_INJECTIONPOINT, wait_event_name);

}

static uint64
WaitEventCustomNew(uint64 classId, const char *wait_event_name)
{
	uint64		eventId;
	bool		found;
	WaitEventCustomEntryByName *entry_by_name;
	WaitEventCustomEntryByInfo *entry_by_info;
	uint64		wait_event_info;

	/* Check the limit of the length of the event name */
	if (strlen(wait_event_name) >= NAMEDATALEN)
		elog(ERROR,
			 "cannot use custom wait event string longer than %u characters",
			 NAMEDATALEN - 1);

	/*
	 * Check if the wait event info associated to the name is already defined,
	 * and return it if so.
	 */
	LWLockAcquire(WaitEventCustomLock, LW_SHARED);
	entry_by_name = (WaitEventCustomEntryByName *)
		hash_search(WaitEventCustomHashByName, wait_event_name,
					HASH_FIND, &found);
	LWLockRelease(WaitEventCustomLock);
	if (found)
	{
		uint64		oldClassId;

		oldClassId = entry_by_name->wait_event_info & WAIT_EVENT_CLASS_MASK;
		if (oldClassId != classId)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("wait event \"%s\" already exists in type \"%s\"",
							wait_event_name,
							pgstat_get_wait_event_type(entry_by_name->wait_event_info))));
		return entry_by_name->wait_event_info | classId;
	}

	/*
	 * Allocate and register a new wait event.  Recheck if the event name
	 * exists, as it could be possible that a concurrent process has inserted
	 * one with the same name since the LWLock acquired again here was
	 * previously released.
	 */
	LWLockAcquire(WaitEventCustomLock, LW_EXCLUSIVE);
	entry_by_name = (WaitEventCustomEntryByName *)
		hash_search(WaitEventCustomHashByName, wait_event_name,
					HASH_FIND, &found);
	if (found)
	{
		uint64		oldClassId;

		LWLockRelease(WaitEventCustomLock);
		oldClassId = entry_by_name->wait_event_info & WAIT_EVENT_CLASS_MASK;
		if (oldClassId != classId)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("wait event \"%s\" already exists in type \"%s\"",
							wait_event_name,
							pgstat_get_wait_event_type(entry_by_name->wait_event_info))));
		return entry_by_name->wait_event_info | classId;
	}

	/* Allocate a new event Id */
	SpinLockAcquire(&WaitEventCustomCounter->mutex);

	if (WaitEventCustomCounter->nextId >= WAIT_EVENT_CUSTOM_HASH_MAX_SIZE)
	{
		SpinLockRelease(&WaitEventCustomCounter->mutex);
		ereport(ERROR,
				errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("too many custom wait events"));
	}

	/*
	 * We shift the resulting nextId it as it needs to land in higher 32-bits
	 * part of the 64-bit word (see WAIT_EVENT_ID_MASK).
	 */
	eventId = (uint64) (WaitEventCustomCounter->nextId++) << 32;

	SpinLockRelease(&WaitEventCustomCounter->mutex);

	/* Register the new wait event */
	wait_event_info = classId | (uint64) eventId;
	entry_by_info = (WaitEventCustomEntryByInfo *)
		hash_search(WaitEventCustomHashByInfo, &wait_event_info,
					HASH_ENTER, &found);
	Assert(!found);
	strlcpy(entry_by_info->wait_event_name, wait_event_name,
			sizeof(entry_by_info->wait_event_name));

	entry_by_name = (WaitEventCustomEntryByName *)
		hash_search(WaitEventCustomHashByName, wait_event_name,
					HASH_ENTER, &found);
	Assert(!found);
	entry_by_name->wait_event_info = wait_event_info;

	LWLockRelease(WaitEventCustomLock);

	return wait_event_info;
}

/*
 * Return the name of a custom wait event information.
 */
static const char *
GetWaitEventCustomIdentifier(uint64 wait_event_info)
{
	bool		found;
	WaitEventCustomEntryByInfo *entry;

	/* Built-in event? */
	if (wait_event_info == PG_WAIT_EXTENSION)
		return "Extension";

	/* It is a user-defined wait event, so lookup hash table. */
	LWLockAcquire(WaitEventCustomLock, LW_SHARED);
	entry = (WaitEventCustomEntryByInfo *)
		hash_search(WaitEventCustomHashByInfo, &wait_event_info,
					HASH_FIND, &found);
	LWLockRelease(WaitEventCustomLock);

	if (!entry)
		elog(ERROR,
			 "could not find custom name for wait event information %" PRIu64,
			 wait_event_info);

	return entry->wait_event_name;
}


/*
 * Returns a list of currently defined custom wait event names.  The result is
 * a palloc'd array, with the number of elements saved in *nwaitevents.
 */
char	  **
GetWaitEventCustomNames(uint64 classId, int *nwaitevents)
{
	char	  **waiteventnames;
	WaitEventCustomEntryByName *hentry;
	HASH_SEQ_STATUS hash_seq;
	int			index;
	int			els;

	LWLockAcquire(WaitEventCustomLock, LW_SHARED);

	/* Now we can safely count the number of entries */
	els = hash_get_num_entries(WaitEventCustomHashByName);

	/* Allocate enough space for all entries */
	waiteventnames = palloc_array(char *, els);

	/* Now scan the hash table to copy the data */
	hash_seq_init(&hash_seq, WaitEventCustomHashByName);

	index = 0;
	while ((hentry = (WaitEventCustomEntryByName *) hash_seq_search(&hash_seq)) != NULL)
	{
		if ((hentry->wait_event_info & WAIT_EVENT_CLASS_MASK) != classId)
			continue;
		waiteventnames[index] = pstrdup(hentry->wait_event_name);
		index++;
	}

	LWLockRelease(WaitEventCustomLock);

	*nwaitevents = index;
	return waiteventnames;
}

/*
 * Configure wait event reporting to report wait events to *wait_event_info.
 * *wait_event_info needs to be valid until pgstat_reset_wait_event_storage()
 * is called.
 *
 * Expected to be called during backend startup, to point my_wait_event_info
 * into shared memory.
 */
void
pgstat_set_wait_event_storage(volatile pg_atomic_uint64 *wait_event_info)
{
	my_wait_event_info = wait_event_info;
}

/*
 * Reset wait event storage location.
 *
 * Expected to be called during backend shutdown, before the location set up
 * pgstat_set_wait_event_storage() becomes invalid.
 */
void
pgstat_reset_wait_event_storage(void)
{
	my_wait_event_info = &local_my_wait_event_info;
}

/* ----------
 * pgstat_get_wait_event_type() -
 *
 *	Return a string representing the current wait event type, backend is
 *	waiting on.
 */
const char *
pgstat_get_wait_event_type(uint64 wait_event_info)
{
	uint64		classId;
	const char *event_type;

	/* report process as not waiting. */
	if (wait_event_info == 0)
		return NULL;

	classId = wait_event_info & WAIT_EVENT_CLASS_MASK;

	switch (classId)
	{
		case PG_WAIT_LWLOCK:
			event_type = "LWLock";
			break;
		case PG_WAIT_LOCK:
			event_type = "Lock";
			break;
		case PG_WAIT_BUFFER:
			event_type = "Buffer";
			break;
		case PG_WAIT_ACTIVITY:
			event_type = "Activity";
			break;
		case PG_WAIT_CLIENT:
			event_type = "Client";
			break;
		case PG_WAIT_EXTENSION:
			event_type = "Extension";
			break;
		case PG_WAIT_IPC:
			event_type = "IPC";
			break;
		case PG_WAIT_TIMEOUT:
			event_type = "Timeout";
			break;
		case PG_WAIT_IO:
			event_type = "IO";
			break;
		case PG_WAIT_INJECTIONPOINT:
			event_type = "InjectionPoint";
			break;
		default:
			event_type = "???";
			break;
	}

	return event_type;
}

/* ----------
 * pgstat_get_wait_event() -
 *
 *	Return a string representing the current wait event, backend is
 *	waiting on.
 */
const char *
pgstat_get_wait_event(uint64 wait_event_info)
{
	uint64		classId;
	uint16		eventId;
	const char *event_name;

	/* report process as not waiting. */
	if (wait_event_info == 0)
		return NULL;

	classId = wait_event_info & WAIT_EVENT_CLASS_MASK;
	eventId = (uint16) ((wait_event_info & WAIT_EVENT_ID_MASK) >> 32);

	switch (classId)
	{
		case PG_WAIT_LWLOCK:
			event_name = GetLWLockIdentifier(classId, eventId);
			break;
		case PG_WAIT_LOCK:
			event_name = GetLockNameFromTagType(eventId);
			break;
		case PG_WAIT_EXTENSION:
		case PG_WAIT_INJECTIONPOINT:
			event_name = GetWaitEventCustomIdentifier(wait_event_info);
			break;
		/* for the below ones we need to mask lower 32-bits */
		case PG_WAIT_BUFFER:
			{
				WaitEventBuffer w = (WaitEventBuffer) wait_event_info & WAIT_EVENT_CLASSID_MASK;

				event_name = pgstat_get_wait_buffer(w);
				break;
			}
		case PG_WAIT_ACTIVITY:
			{
				WaitEventActivity w = (WaitEventActivity) wait_event_info & WAIT_EVENT_CLASSID_MASK;


				event_name = pgstat_get_wait_activity(w);
				break;
			}
		case PG_WAIT_CLIENT:
			{
				WaitEventClient w = (WaitEventClient) wait_event_info & WAIT_EVENT_CLASSID_MASK;


				event_name = pgstat_get_wait_client(w);
				break;
			}
		case PG_WAIT_IPC:
			{
				WaitEventIPC w = (WaitEventIPC) wait_event_info & WAIT_EVENT_CLASSID_MASK;


				event_name = pgstat_get_wait_ipc(w);
				break;
			}
		case PG_WAIT_TIMEOUT:
			{
				WaitEventTimeout w = (WaitEventTimeout) wait_event_info & WAIT_EVENT_CLASSID_MASK;

				event_name = pgstat_get_wait_timeout(w);
				break;
			}
		case PG_WAIT_IO:
			{
				WaitEventIO w = (WaitEventIO) wait_event_info & WAIT_EVENT_CLASSID_MASK;

				event_name = pgstat_get_wait_io(w);
				break;
			}
		default:
			event_name = "unknown wait event";
			break;
	}

	return event_name;
}

uint32
pgstat_get_wait_event_arg(uint64 wait_event_info)
{
	uint32		arg = wait_event_info & WAIT_EVENT_ARG_MASK;
	return arg;
}

#include "utils/pgstat_wait_event.c"
