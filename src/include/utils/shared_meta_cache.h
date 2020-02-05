/*-------------------------------------------------------------------------
 *
 * shared_meta_cache.h
 *	  Routines for shared catalog/relation cache
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/shared_meta_cache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHARED_META_CACHE_H
#define SHARED_META_CACHE_H

#include "lib/ilist.h"
#include "lib/dshash.h"
#include "storage/lwlock.h"
#include "utils/catcache.h"

/* Number of partitions of the shared cache mapping hashtable */
#define NUM_MAP_PARTITIONS  8
#define IS_GLOBAL_META_CACHE (shared_metacache_mem != 0)

/* invalidation phase used in shared catalog/relation cache */
typedef enum InvalPhase
{
	INVAL_COMMON, /* 1. when meta cache is not shared
				   * 2. when sinval is read */
	INVAL_GMC_COMMIT_XACT, /* when transaction is committed */
	INVAL_GMC_ABORT_XACT,
	INVAL_GMC_COMMIT_SUBXACT,
	INVAL_GMC_ABORT_SUBXACT, /* when sub-transaction is aborted */
	INVAL_GMC_EO_COMMAND /* end of command */
} InvalPhase;


/*
 * Declarations for cache handle
 */
typedef struct HandleHeader HandleHeader;
typedef struct HandleBlock HandleBlock;
typedef struct Handle Handle;

struct Handle
{
	bool free; 			/* is chunk free? */
	int generation;     /* how many times this chunk is recycled */
	HandleBlock *block; /* block owning this chunk */
	void *entry; 	/* link to global catalog/relation cache entry */
};

struct HandleBlock
{
	dlist_node node; /* dlist node */
	uint32 nfree; 	/* number of free handles */
	bool infreelist;   	/* if it's a member of freelist or not */
	slock_t mutex; /* lock for Handle members and above except for infreelist */
#define HANDLE_BLOCK_SIZE 32
	Handle handles[HANDLE_BLOCK_SIZE];
	LWLock handle_lock; /* lock for get/invalidate handle operation */
};

struct HandleHeader
{
	dlist_head freelist;  /* head of the free block list */
	LWLock freelist_lock; /* lock for freelist and HandleBlock->infreelist */
	void *owner; /* Global Hash table of either catcache or relcache */
};

/*
 * Declarations for shared catalog cache map
 */

/* If CatCache is shared across database, it's pushed to list whose dbid is 0 */
typedef struct GlobalCatCacheMapKey
{
	Oid dbId; 		/* database oid, if shared cache, 0 */
	int cacheId; 	/* catcache id */
} GlobalCatCacheMapKey;

typedef struct GlobalCatCacheMapEnt
{
	GlobalCatCacheMapKey key; /* tag of a CatCache hash table */
	GlobalCatCache *gcp;   /* associated CatCache hash table */
} GlobalCatCacheMapEnt;

/* LWLock for each partition */
typedef struct GlobalCatCacheMapLock
{
	LWLock locks[NUM_MAP_PARTITIONS];
} GlobalCatCacheMapLock;

extern GlobalCatCacheMapLock *globalCatCacheMapLock;

/*
 * Declarations for shared relation cache map
 */
typedef Oid GlobalRelCacheMapKey;

typedef struct GlobalRelCacheMapEnt
{
	GlobalRelCacheMapKey key; /* key of a RelCache hash table */
	dshash_table_handle global_relcache_handle; /* RelCache hash table handle */
	HandleHeader *cache_handle_header; /* header for handles */
} GlobalRelCacheMapEnt;

typedef struct GlobalRelCacheMapLock
{
	LWLock locks[NUM_MAP_PARTITIONS];
} GlobalRelCacheMapLock;

extern GlobalRelCacheMapLock *globalRelCacheMapLock;

/* GUC parameter: 0 indicates catalog/relation cache is built per backend */
extern int shared_metacache_mem;

/* Unit is megabyte */
#define SHM_METACACHE_SIZE_MB ((size_t)(1024 * 1024 * shared_metacache_mem))
#define GET_HANDLE_LOCK(handle) (&handle->block->handle_lock)

/*
 * The shared cache mapping table is partitioned to reduce contention.
 * To determine which partition lock a given key requires, compute the key's
 * hash code with GlobalCatCacheMapHashCode(), then apply
 * SharedCachePartitionLock().
 * NB: NUM_MAP_PARTITIONS must be a power of 2!
 */
#define GlobalCacheMapHashPartition(hashcode) \
	((hashcode) % NUM_MAP_PARTITIONS)
#define GlobalCatCachePartitionLock(hashcode) \
	(&globalCatCacheMapLock->locks[GlobalCacheMapHashPartition(hashcode)])
#define GlobalRelCachePartitionLock(hashcode) \
	(&globalRelCacheMapLock->locks[GlobalCacheMapHashPartition(hashcode)])

extern void GlobalMetaCacheShmemInit(void);
extern Size GlobalMetaCacheShmemSize(void);

/* routines for cache handle */
extern void InitCacheHandle(HandleHeader *header, void *owner);
extern void InvalCacheHandle(HandleHeader *header, Handle *chunk);
extern Handle *GetCacheHandle(HandleHeader *header, void *entry);

/* routines for shared hash map */
extern void GlobalCatCacheMapInitKey(CatCache *lcp,
									 GlobalCatCacheMapKey *keyPtr);

extern uint32 GlobalCatCacheMapHashCode(GlobalCatCacheMapKey *keyPtr);

extern GlobalCatCacheMapEnt*
GlobalCatCacheMapLookup(GlobalCatCacheMapKey *keyPtr, uint32 hashcode);

extern bool
GlobalCatCacheMapInsert(GlobalCatCacheMapKey *keyPtr,
						uint32 hashcode,
						GlobalCatCache *gcp);

extern uint32
GlobalRelCacheMapHashCode(GlobalRelCacheMapKey *keyPtr);

extern GlobalRelCacheMapEnt*
GlobalRelCacheMapLookup(GlobalRelCacheMapKey *keyPtr, uint32 hashcode);

extern bool
GlobalRelCacheMapInsert(GlobalRelCacheMapKey *keyPtr,
						uint32 hashcode,
						dshash_table_handle global_relcache_handle,
						HandleHeader *cache_handle_header);

#endif							/* SHARED_META_CACHE_H */
