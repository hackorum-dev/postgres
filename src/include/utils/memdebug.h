/*-------------------------------------------------------------------------
 *
 * memdebug.h
 *	  Memory debugging support.
 *
 * Currently, this file either wraps <valgrind/memcheck.h> or substitutes
 * empty definitions for Valgrind client request macros we use.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/memdebug.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MEMDEBUG_H
#define MEMDEBUG_H


#ifdef USE_VALGRIND

#include <valgrind/memcheck.h>

#else							/* !USE_VALGRIND */
#define VALGRIND_CHECK_MEM_IS_DEFINED(addr, size)			do {} while (0)
#define VALGRIND_CREATE_MEMPOOL(context, redzones, zeroed)	do {} while (0)
#define VALGRIND_DESTROY_MEMPOOL(context)					do {} while (0)
#define VALGRIND_MAKE_MEM_DEFINED(addr, size)				do {} while (0)
#define VALGRIND_MAKE_MEM_NOACCESS(addr, size)				do {} while (0)
#define VALGRIND_MAKE_MEM_UNDEFINED(addr, size)				do {} while (0)
#define VALGRIND_MEMPOOL_ALLOC(context, addr, size)			do {} while (0)
#define VALGRIND_MEMPOOL_FREE(context, addr)				do {} while (0)
#define VALGRIND_MEMPOOL_CHANGE(context, optr, nptr, size)	do {} while (0)
#define VALGRIND_MEMPOOL_TRIM(context, addr, size)			do {} while (0)
#endif							/* !USE_VALGRIND */


#ifdef USE_ASAN

#define ASAN_DEFINE_REGION_MACROS
#include <sanitizer/asan_interface.h>

#else							/* !__SANITIZE_ADDRESS__ */

#define ASAN_POISON_MEMORY_REGION(addr, size)				do {} while (0)
#define ASAN_UNPOISON_MEMORY_REGION(addr, size)				do {} while (0)

#endif							/* !__SANITIZE_ADDRESS__ */


#define PG_ANNOTATE_MEM_DEFINED(addr, size)	\
	do { \
		VALGRIND_MAKE_MEM_DEFINED(addr, size); \
		ASAN_UNPOISON_MEMORY_REGION(addr, size); \
	} while (0)

#define PG_ANNOTATE_MEM_NOACCESS(addr, size) \
	do { \
		VALGRIND_MAKE_MEM_NOACCESS(addr, size); \
		ASAN_POISON_MEMORY_REGION(addr, size); \
	} while (0)

#define PG_ANNOTATE_MEM_UNDEFINED(addr, size) \
	do { \
		VALGRIND_MAKE_MEM_UNDEFINED(addr, size); \
		ASAN_UNPOISON_MEMORY_REGION(addr, size); \
	} while (0)

#define PG_ANNOTATE_MEMPOOL_ALLOC(context, addr, size) \
	do { \
		VALGRIND_MEMPOOL_ALLOC(context, addr, size); \
		ASAN_UNPOISON_MEMORY_REGION(addr, size); \
	} while (0)

#define PG_ANNOTATE_MEMPOOL_FREE(context, addr) \
	do { \
		VALGRIND_MEMPOOL_FREE(context, addr); \
		/* XXX: without size we can't do anything for asan */ \
	} while (0)

#define PG_ANNOTATE_MEMPOOL_CHANGE(context, optr, nptr, size) \
	do { \
		VALGRIND_MEMPOOL_CHANGE(context, optr, nptr, size); \
		/* XXX: probably nothing to do here for asan? */ \
	} while (0)

#define PG_ANNOTATE_MEMPOOL_TRIM(context, addr, size) \
	do { \
		VALGRIND_MEMPOOL_TRIM(context, addr, size); \
		/* XXX: probably nothing to do here for asan? */ \
	} while (0)


#ifdef CLOBBER_FREED_MEMORY

/* Wipe freed memory for debugging purposes */
static inline void
wipe_mem(void *ptr, size_t size)
{
	PG_ANNOTATE_MEM_UNDEFINED(ptr, size);
	memset(ptr, 0x7F, size);
	PG_ANNOTATE_MEM_NOACCESS(ptr, size);
}

#endif							/* CLOBBER_FREED_MEMORY */

#ifdef MEMORY_CONTEXT_CHECKING

static inline void
set_sentinel(void *base, Size offset)
{
	char	   *ptr = (char *) base + offset;

	PG_ANNOTATE_MEM_UNDEFINED(ptr, MEMORY_CONTEXT_SENTINEL_SIZE);
	memset(ptr, 0x7E, MEMORY_CONTEXT_SENTINEL_SIZE);
	PG_ANNOTATE_MEM_NOACCESS(ptr, MEMORY_CONTEXT_SENTINEL_SIZE);
}

static inline bool
sentinel_ok(const void *base, Size offset)
{
	const char *ptr = (const char *) base + offset;
	bool		ret = true;

	PG_ANNOTATE_MEM_DEFINED(ptr, MEMORY_CONTEXT_SENTINEL_SIZE);
	for (Size i = 0; i < MEMORY_CONTEXT_SENTINEL_SIZE; i++)
	{
		if (*ptr++ != 0x7e)
		{
			ret = false;
			break;
		}
	}
	PG_ANNOTATE_MEM_NOACCESS(ptr, MEMORY_CONTEXT_SENTINEL_SIZE);

	return ret;
}

#endif							/* MEMORY_CONTEXT_CHECKING */

#ifdef RANDOMIZE_ALLOCATED_MEMORY

void		randomize_mem(char *ptr, size_t size);

#endif							/* RANDOMIZE_ALLOCATED_MEMORY */


#endif							/* MEMDEBUG_H */
