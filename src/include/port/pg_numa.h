/*-------------------------------------------------------------------------
 *
 * pg_numa.h
 *	  Basic NUMA portability routines
 *
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 * 	src/include/port/pg_numa.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_NUMA_H
#define PG_NUMA_H

// JW: is this legal to be included here?
#include <numa.h>
#include <numaif.h>

typedef struct bitmask pg_numa_bitmask_t;

extern PGDLLIMPORT int pg_numa_init(void);
extern PGDLLIMPORT int pg_numa_query_pages(int pid, unsigned long count, void **pages, int *status);
extern PGDLLIMPORT int pg_numa_get_max_node(void);
extern PGDLLIMPORT int pg_numa_interleave_memptr(void *ptr, size_t sz, pg_numa_bitmask_t *mask);
extern PGDLLIMPORT pg_numa_bitmask_t *pg_numa_parse_nodestring(const char *string);
extern PGDLLIMPORT void pg_numa_set_bind_policy(int strict);
extern PGDLLIMPORT void pg_numa_bind(pg_numa_bitmask_t *nodemask);

#ifdef USE_LIBNUMA

/*
 * This is required on Linux, before pg_numa_query_pages() as we
 * need to page-fault before move_pages(2) syscall returns valid results.
 */
#define pg_numa_touch_mem_if_required(ro_volatile_var, ptr) \
	ro_volatile_var = *(volatile uint64 *) ptr

extern void numa_warn(int num, char *fmt,...) pg_attribute_printf(2, 3);
extern void numa_error(char *where);

#else

#define pg_numa_touch_mem_if_required(ro_volatile_var, ptr) \
	do {} while(0)

#endif

#endif							/* PG_NUMA_H */
