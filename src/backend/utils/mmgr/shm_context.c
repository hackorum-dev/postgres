/*-------------------------------------------------------------------------
 *
 * shm_context.c
 *	  General utilities for ShmRetailContext and ShmZoneContext.
 *
 * Both ShmRetailContext and ShmZoneContext is a MemoryContext implementation
 * designed for cases where you want to allocate and free objects in the
 * shared memory by MemoryContext API. This file provides utilities for
 * both implementations.
 *
 * Portions Copyright (c) 2017-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/shm_context.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/shmem.h"
#include "utils/dsa.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"

/*
 * Global meta cache area. Note that dsa_area is allocated in backend and
 * can not be shared among backends.
 */
static void *gmc_raw_area;
static dsa_area *gmc_dsa_area;

/*
 * Genreral type for Shared MemoryContext.
 * See also ShmRetailContext and ShmZoneContext
 */
typedef struct ShmContext
{
	MemoryContextData header;	/* Standard memory-context fields */
	/* ShmRetailContext parameters */
	LWLock *child_lock;		   	/* protect child graph path */
} ShmContext;

/*
 * Create GMC area and map it with GlobalCacheContext backed by DSA
 */
void
GlobalCacheContextShmemInit(Size size)
{
	MemoryContext	old_context;
	bool	found;

	/* Life span of dsa_area is same as this process */
	old_context = MemoryContextSwitchTo(TopMemoryContext);

	/* Allocate, or look up, a chunk of raw fixed-address shared memory. */
	gmc_raw_area = ShmemInitStruct("global_meta_cache", size, &found);

	if (!found)
	{
		/*
		 * Create a new DSA area, and clamp its size so it can't make any
		 * segments outside the provided space.
		 */
		gmc_dsa_area = dsa_create_in_place(gmc_raw_area, size, 0, NULL);

		dsa_set_size_limit(gmc_dsa_area, size);

		/* To prevent the area from being released */
		dsa_pin(gmc_dsa_area);
	}
	else
	{
		/* Attach to an existing area and get dsa_area */
		gmc_dsa_area = dsa_attach_in_place(gmc_raw_area, NULL);
	}

	/*
	 * Set dsa_area of GMC so that other shared MemoryContext can use it
	 */
	ShmRetailContextSetDSAarea(gmc_dsa_area);
	ShmZoneContextSetDSAarea(gmc_dsa_area);

	/* Create a shared memory context */
	GlobalCacheContext
		= ShmRetailContextCreateGlobal(NULL,
									   "global_cache_context",
									   gmc_raw_area);

	MemoryContextSwitchTo(old_context);
}

void
GlobalCatCacheContextShmemInit(void)
{
	/* Create or attach to shared memory context */
	GlobalCatCacheContext
		= ShmRetailContextCreateGlobal(GlobalCacheContext,
									   "global_catcache_context",
									   gmc_raw_area);
}

void
GlobalRelCacheContextShmemInit(void)
{
	/* Create or attach to shared memory context */
	GlobalRelCacheContext
		= ShmZoneContextCreateOrigin(GlobalCacheContext,
									 "global_relcache_context",
									 gmc_raw_area);
}

/*
 * Return size of GlobalCacheContext
 */
Size
GlobalCacheContextSize(void)
{
	return ShmRetailContextSize();
}

/*
 * Return size of GlobalCatCacheContext
 */
Size
GlobalCatCacheContextSize(void)
{
	return ShmRetailContextSize();
}

/*
 * Return size of GlobalRelCacheContext
 */
Size
GlobalRelCacheContextSize(void)
{
	return ShmZoneContextSize();
}

/*
 * General function for get context child path lock
 */
LWLock *
ShmContextGetLock(MemoryContext context)
{
	Assert(MemoryContextIsShared(context));

	return ((ShmContext *)context)->child_lock;
}
