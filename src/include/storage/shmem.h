/*-------------------------------------------------------------------------
 *
 * shmem.h
 *	  shared memory management structures
 *
 * Historical note:
 * A long time ago, Postgres' shared memory region was allowed to be mapped
 * at a different address in each process, and shared memory "pointers" were
 * passed around as offsets relative to the start of the shared memory region.
 * That is no longer the case: each process must map the shared memory region
 * at the same address.  This means shared memory pointers can be passed
 * around directly between different processes.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/shmem.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHMEM_H
#define SHMEM_H

#include "storage/spin.h"
#include "utils/hsearch.h"


/* shmem.c */
typedef struct PGShmemHeader PGShmemHeader; /* avoid including
											 * storage/pg_shmem.h here */
extern void InitShmemAllocator(PGShmemHeader *seghdr);
extern void *ShmemAlloc(Size size);
extern void *ShmemAllocNoError(Size size);
extern bool ShmemAddrIsValid(const void *addr);
extern HTAB *ShmemInitHash(const char *name, int64 init_size, int64 max_size,
						   HASHCTL *infoP, int hash_flags);
extern void *ShmemInitStruct(const char *name, Size size, bool *foundPtr);
extern Size add_size(Size s1, Size s2);
extern Size mul_size(Size s1, Size s2);

extern PGDLLIMPORT Size pg_get_shmem_pagesize(void);

/*
 * Simplified shared memory hash table creation API
 *
 * These macros provide a simpler way to create shared memory hash tables by:
 * - Automatically determining keysize and entrysize from type information
 * - Automatically choosing HASH_STRINGS vs HASH_BLOBS based on key type
 * - Eliminating the need for explicit HASHCTL and flags in common cases
 *
 * Usage:
 *   HTAB *hash = shmem_hash_make(MyEntry, keyfield, "My hash", 64, 128);
 *
 * For more options (partitioning, fixed size, custom hash):
 *   HASHOPTS opts = {.num_partitions = 16, .fixed_size = true};
 *   HTAB *hash = shmem_hash_make_ext(MyEntry, keyfield, "My hash", 64, 128, &opts);
 */
#define shmem_hash_make(entrytype, keymember, tabname, init_size, max_size) \
	shmem_hash_make_ext(entrytype, keymember, tabname, init_size, max_size, NULL)

#define shmem_hash_make_ext(entrytype, keymember, tabname, init_size, max_size, opts) \
	(StaticAssertExpr(offsetof(entrytype, keymember) == 0, \
						 #keymember " must be first member in " #entrytype), \
	shmem_hash_make_impl( \
		(tabname), (init_size), (max_size), \
		sizeof(((entrytype *)0)->keymember), \
		sizeof(entrytype), \
		HASH_KEY_AS_STRING(entrytype, keymember), \
		(opts)))

extern HTAB *shmem_hash_make_impl(const char *name, int64 init_size, int64 max_size,
								  Size keysize, Size entrysize, bool string_key,
								  const HASHOPTS *opts);

/* ipci.c */
extern void RequestAddinShmemSpace(Size size);

/* size constants for the shmem index table */
 /* max size of data structure string name */
#define SHMEM_INDEX_KEYSIZE		 (48)
 /* estimated size of the shmem index table (not a hard limit) */
#define SHMEM_INDEX_SIZE		 (64)

/* this is a hash bucket in the shmem index table */
typedef struct
{
	char		key[SHMEM_INDEX_KEYSIZE];	/* string name */
	void	   *location;		/* location in shared mem */
	Size		size;			/* # bytes requested for the structure */
	Size		allocated_size; /* # bytes actually allocated */
} ShmemIndexEnt;

#endif							/* SHMEM_H */
