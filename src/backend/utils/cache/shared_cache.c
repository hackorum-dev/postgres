/*-------------------------------------------------------------------------
 *
 * shared_cache.c
 *	  Utilities for global system catalog cache and global relation cache
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/globalcache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/shared_meta_cache.h"
#include "utils/syscache.h"

#include "miscadmin.h"


#define NUM_DB 16 /* arbiary number */

/* shared cache map to find the global catalog or relation hash table */
static HTAB *GlobalCatCacheMap;
static HTAB *GlobalRelCacheMap;

//static MemoryContext TempGlobalCacheContext = NULL;

static HandleBlock *InitHandleBlock(void);

static void InitGlobalCatCacheMap(void);
static void InitGlobalRelCacheMap(void);
static Size GlobalCatCacheMapSize(void);
static Size GlobalRelCacheMapSize(void);

GlobalCatCacheMapLock *globalCatCacheMapLock = NULL;
GlobalRelCacheMapLock *globalRelCacheMapLock = NULL;

/*
 * --------------------------------------------------------------
 * Utilities for cache handle
 * --------------------------------------------------------------
 */

/*
 * InitCacheHanlde()
 * 		Initialize HandleHeader with one free block.
 *
 */
void
InitCacheHandle(HandleHeader *header, void *owner)
{
	HandleBlock *block;

	header->owner = owner;

	LWLockInitialize(&header->freelist_lock, LWTRANCHE_CACHE_HANDLE);

	dlist_init(&header->freelist);

	/* Caller should switch to appropriate shared context */
	block = InitHandleBlock();

	dlist_push_head(&header->freelist, &block->node);
}

static HandleBlock *
InitHandleBlock(void)
{
	HandleBlock *block;
	int i;

	Assert(IsA((CurrentMemoryContext), ShmRetailContext) ||
		   IsA((CurrentMemoryContext), ShmZoneContext));

	/* Assume MemoryContext is already switched */
	block = (HandleBlock *) palloc0(sizeof(HandleBlock));

	block->nfree = HANDLE_BLOCK_SIZE;

	block->infreelist = true;

	/* Initialize locks for partition of hash table */
	SpinLockInit(&block->mutex);

	LWLockInitialize(&block->handle_lock, LWTRANCHE_META_CACHE_HANDLE);

	/* Initailize handle handles */
	for (i = 0; i < HANDLE_BLOCK_SIZE; ++i)
	{
		block->handles[i].free = true;
		block->handles[i].block = block;
	}
	return block;
}


/*
 * InvalCacheHandle()
 *  Free the used handle to recycle it.
 *	When block is not in freelist, push the block to tail of list.
 */
void
InvalCacheHandle(HandleHeader *header, Handle *handle)
{
	/* Get lock for the handle and its block */
	SpinLockAcquire(&handle->block->mutex);

	/* If this function is concurrently called, the handle might be already freed */
	if (handle->free)
	{
		SpinLockRelease(&handle->block->mutex);
		return;
	}

	handle->free = true;
	handle->generation++;
	handle->entry = NULL;
	handle->block->nfree++;
	SpinLockRelease(&handle->block->mutex);

	/*
	 * If the block is already in the freelist, do nothing and exit.
	 * If not, push the block to the list. Check the block is in the list
	 * or not twice to reduce the locking window.
	 */
	if (!handle->block->infreelist)
	{
		LWLockAcquire(&header->freelist_lock, LW_EXCLUSIVE);

		/*
		 * The block might be already pushed concurrently. There is also
		 * possibility that someone already got the free handle before get LWLock.
		 */
		if (!handle->block->infreelist && handle->block->nfree != 0)
		{
			handle->block->infreelist = true;
			dlist_push_tail(&header->freelist, &handle->block->node);
		}
		LWLockRelease(&header->freelist_lock);
	}
}


/*
 * GetCacheHandle()
 * 	Return Handle. If not found, build a new free block.
 */
Handle *
GetCacheHandle(HandleHeader *header, void *entry)
{
	Handle *handle = NULL;
	HandleBlock *block = NULL;
	int i;

	LWLockAcquire(&header->freelist_lock, LW_SHARED);

	/*
	 * Get the first block of freelist. If no available free block, build a new
	 * one. To reduce the lock contention, exclusive locking duration is as
	 * small as possible.  Because a newly added block may become full, we
	 * check if freelist is empyt recursively.
	 */
	while (dlist_is_empty(&header->freelist))
	{
		MemoryContext old_context;

		LWLockRelease(&header->freelist_lock);

		/* Append the newly built block */
		LWLockAcquire(&header->freelist_lock, LW_EXCLUSIVE);

		/* TempGlobalCacheContext = MemoryContextClone(GlobalCacheContext, */
		/* 											CurTransactionContext); */

		/* old_context = MemoryContextSwitchTo(TempGlobalCacheContext); */

		old_context = MemoryContextSwitchTo(GlobalCacheContext);

		block = InitHandleBlock();

		MemoryContextSwitchTo(old_context);

		/* ShmRetailContextMoveChunk(TempGlobalCacheContext, GlobalCacheContext); */

		dlist_push_head(&header->freelist, &block->node);

		LWLockRelease(&header->freelist_lock);

		LWLockAcquire(&header->freelist_lock, LW_SHARED);
	}

	block = dlist_head_element(HandleBlock, node, &header->freelist);

	/* Search linearly to find free handle */
	for (i = 0; i < HANDLE_BLOCK_SIZE; ++i)
	{
		if (!block->handles[i].free)
			continue;

		/* Get lock for handle and its owning block */
		SpinLockAcquire(&block->mutex);

		/* Check the handle again because it might be already occupied */
		if (!block->handles[i].free)
		{
			SpinLockRelease(&block->mutex);
			continue;
		}

		block->nfree--;
		block->handles[i].generation++;
		block->handles[i].free = false;
		handle = &block->handles[i];

		SpinLockRelease(&block->mutex);

		break;
	}

	LWLockRelease(&header->freelist_lock);


	/*
	 * If the block becomes full, delete from free list and mark it. To reduce
	 * the lock duration, nfree is checked before lock is acquired and checked
	 * again after acquation of lock.
	 */
	if (block->nfree == 0)
	{
		LWLockAcquire(&header->freelist_lock, LW_EXCLUSIVE);

		if (block->infreelist && block->nfree == 0)
		{
			block->infreelist = false;
			dlist_delete(&block->node);
		}
		LWLockRelease(&header->freelist_lock);
	}

	/* Set link from handle to entry */
	handle->entry = entry;

	return handle;
}

/*
 * --------------------------------------------------------------
 * Utilities for both global catalog and relation cache
 * --------------------------------------------------------------
 */

/*
 * GlobalMetaCacheShmemInit
 *
 * This is called during shared-memory initialization
 *
 */
void
GlobalMetaCacheShmemInit(void)
{
	/* do nothing if catalog cache is not global */
	if (!IS_GLOBAL_META_CACHE)
		return;

	/* Create GMC area and GlobalCacheContext */
	GlobalCacheContextShmemInit(SHM_METACACHE_SIZE_MB);

	/* Initialize child contexts of GlobalCacheContext */
	GlobalCatCacheContextShmemInit();
	GlobalRelCacheContextShmemInit();

	/* Initialize Maps to associate tag with CatCache and Relation hash table */
	InitGlobalCatCacheMap();
	InitGlobalRelCacheMap();
}

Size
GlobalMetaCacheShmemSize(void)
{
	Size size = 0;

	/* do nothing if catalog cache is not shared */
	if (!IS_GLOBAL_META_CACHE)
		return 0;

	/* size of global catalog cache area */
	size = add_size(size, SHM_METACACHE_SIZE_MB);

	/* size of shared MemoryContext */
	size = add_size(size, GlobalCacheContextSize());
	size = add_size(size, GlobalCatCacheContextSize());
	size = add_size(size, GlobalRelCacheContextSize());

	/* size of lookup hash table */
	size = add_size(size, GlobalCatCacheMapSize());
	size = add_size(size, GlobalRelCacheMapSize());

	return size;
}


/*
 * --------------------------------------------------------------
 * Utilities for hash table map of global catalog cache
 * --------------------------------------------------------------
 */

static void
InitGlobalCatCacheMap(void)
{
	HASHCTL info;
	size_t init_size;
	size_t max_size;
	bool found;
	int i;

	/* assume no locking is needed yet */

	info.keysize = sizeof(GlobalCatCacheMapKey);
	info.entrysize = sizeof(GlobalCatCacheMapEnt);
	info.num_partitions = NUM_MAP_PARTITIONS;

	/*
	 * Number of CatCache table times DB. Actually, this is slightly more than
	 * we need because it counts dublicatedly shared catalog cache.
	 */
	init_size = max_size = SysCacheSize * NUM_DB;


	GlobalCatCacheMap = ShmemInitHash("Global CatCache Map",
									  init_size, max_size,
									  &info,
									  HASH_ELEM | HASH_BLOBS |HASH_PARTITION);

	/* Get the pointer to LWLocks for parition */
	globalCatCacheMapLock = (GlobalCatCacheMapLock *)
		ShmemInitStruct("GlobalCatCacheMapLock",
						sizeof(GlobalCatCacheMapLock),	&found);

	if (!found)
	{
		Assert(!IsUnderPostmaster);

		for (i = 0; i < NUM_MAP_PARTITIONS; i++)
			LWLockInitialize(&globalCatCacheMapLock->locks[i],
							 LWTRANCHE_META_CACHE_MAP);
	}
}

/*
 * Estimate space needed for mapping hashtable
 */
static Size
GlobalCatCacheMapSize(void)
{
	/* size of lookup hash table. see comments in InitGlobalCatCacheMap */
	return hash_estimate_size((SysCacheSize * NUM_DB), sizeof(GlobalCatCacheMapEnt));
}

uint32
GlobalCatCacheMapHashCode(GlobalCatCacheMapKey *keyPtr)
{
	return get_hash_value(GlobalCatCacheMap, (void *) keyPtr);
}


GlobalCatCacheMapEnt*
GlobalCatCacheMapLookup(GlobalCatCacheMapKey *keyPtr, uint32 hashcode)
{
	GlobalCatCacheMapEnt *result;

	result = (GlobalCatCacheMapEnt *)
		hash_search_with_hash_value(GlobalCatCacheMap,
									(void *) keyPtr,
									hashcode,
									HASH_FIND,
									NULL);

	if (!result)
		return NULL;

	return result;
}

bool
GlobalCatCacheMapInsert(GlobalCatCacheMapKey *keyPtr,
						uint32 hashcode, GlobalCatCache *gcp)
{
	GlobalCatCacheMapEnt *result;
	bool found;

	result = (GlobalCatCacheMapEnt *)
		hash_search_with_hash_value(GlobalCatCacheMap,
									(void *) keyPtr,
									hashcode,
									HASH_ENTER,
									&found);

	if (found)
		return false;

	result->gcp = gcp;

	return true;
}

void
GlobalCatCacheMapInitKey(CatCache *lcp, GlobalCatCacheMapKey *keyPtr)
{
	Assert(lcp->cc_type == CC_SHAREDLOCAL);

	keyPtr->dbId = lcp->cc_relisshared ? (Oid) 0 : MyDatabaseId;
	keyPtr->cacheId = lcp->id;
}

/*
 * --------------------------------------------------------------
 * Utilities for hash table map of global catalog cache
 * --------------------------------------------------------------
 */
static void
InitGlobalRelCacheMap(void)
{
	HASHCTL info;
	size_t init_size;
	size_t max_size;
	bool found;
	int i;

	/* assume no locking is needed yet */
	info.keysize = sizeof(GlobalRelCacheMapKey);
	info.entrysize = sizeof(GlobalRelCacheMapEnt);
	info.num_partitions = NUM_MAP_PARTITIONS;

	init_size = NUM_DB;
	max_size = 2 * NUM_DB;

	GlobalRelCacheMap = ShmemInitHash("Global Relcache Map",
									  init_size, max_size,
									  &info,
									  HASH_ELEM | HASH_BLOBS |HASH_PARTITION);

	/* Get the pointer to LWLocks for parition */
	globalRelCacheMapLock = (GlobalRelCacheMapLock *)
		ShmemInitStruct("GlobalRelacacheMapLock",
						sizeof(GlobalRelCacheMapLock),&found);

	if (!found)
	{
		Assert(!IsUnderPostmaster);

		for (i = 0; i < NUM_MAP_PARTITIONS; i++)
			LWLockInitialize(&globalRelCacheMapLock->locks[i],
							 LWTRANCHE_META_CACHE_MAP);
	}
}

/*
 * Estimate space needed for mapping hashtable
 */
static Size
GlobalRelCacheMapSize(void)
{
	size_t max_size = 2 * NUM_DB;
	return hash_estimate_size(max_size, sizeof(GlobalRelCacheMapEnt));
}

uint32
GlobalRelCacheMapHashCode(GlobalRelCacheMapKey *keyPtr)
{
	return get_hash_value(GlobalRelCacheMap, (void *) keyPtr);
}


GlobalRelCacheMapEnt*
GlobalRelCacheMapLookup(GlobalRelCacheMapKey *keyPtr, uint32 hashcode)
{
	GlobalRelCacheMapEnt *result;

	result = (GlobalRelCacheMapEnt *)
		hash_search_with_hash_value(GlobalRelCacheMap,
									(void *) keyPtr,
									hashcode,
									HASH_FIND,
									NULL);

	if (!result)
		return NULL;

	return result;
}

bool
GlobalRelCacheMapInsert(GlobalRelCacheMapKey *keyPtr,
						uint32 hashcode,
						dshash_table_handle global_relcache_handle,
						HandleHeader *cache_handle_header)
{
	GlobalRelCacheMapEnt *result;
	bool found;

	result = (GlobalRelCacheMapEnt *)
		hash_search_with_hash_value(GlobalRelCacheMap,
									(void *) keyPtr,
									hashcode,
									HASH_ENTER,
									&found);

	if (found)
		return false;

	result->global_relcache_handle = global_relcache_handle;
	result->cache_handle_header = cache_handle_header;

	return true;
}
