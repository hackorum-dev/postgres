/*-------------------------------------------------------------------------
 *
 * pg_numa.c
 * 		Basic NUMA portability routines
 *
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_numa.c
 *
 *-------------------------------------------------------------------------
 */

//JW:is this legal to replace "c.h" with below:
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <unistd.h>

#include "port/pg_numa.h"
#include "common/string.h"

/*
 * At this point we provide support only for Linux thanks to libnuma, but in
 * future support for other platforms e.g. Win32 or FreeBSD might be possible
 * too. For Win32 NUMA APIs see
 * https://learn.microsoft.com/en-us/windows/win32/procthread/numa-support
 */
#ifdef USE_LIBNUMA

#include <numa.h>
#include <numaif.h>

/* libnuma requires initialization as per numa(3) on Linux */
int
pg_numa_init(void)
{
	int			r = numa_available();

	return r;
}

/*
 * We use move_pages(2) syscall here - instead of get_mempolicy(2) - as the
 * first one allows us to batch and query about many memory pages in one single
 * giant system call that is way faster.
 */
int
pg_numa_query_pages(int pid, unsigned long count, void **pages, int *status)
{
	return numa_move_pages(pid, count, pages, NULL, status, 0);
}

int
pg_numa_get_max_node(void)
{
	return numa_max_node();
}

int
pg_numa_interleave_memptr(void *ptr, size_t sz, pg_numa_bitmask_t *mask)
{
	numa_interleave_memory(ptr, sz, mask);
	return 0;
}

pg_numa_bitmask_t *
pg_numa_parse_nodestring(const char *string)
{
	return numa_parse_nodestring(string);
}

void
pg_numa_set_bind_policy(int strict)
{
	numa_set_bind_policy(strict);
}

void
pg_numa_bind(pg_numa_bitmask_t *nodemask)
{
	numa_bind(nodemask);
}

#ifndef FRONTEND
/*
 * The standard libnuma built-in code can be seen here:
 * https://github.com/numactl/numactl/blob/master/libnuma.c
 *
 */
void
numa_warn(int num, char *fmt,...)
{
	va_list		ap;
	int			olde = errno;
	int			needed;
	StringInfoData msg;

	initStringInfo(&msg);

	va_start(ap, fmt);
	needed = appendStringInfoVA(&msg, fmt, ap);
	va_end(ap);
	if (needed > 0)
	{
		enlargeStringInfo(&msg, needed);
		va_start(ap, fmt);
		appendStringInfoVA(&msg, fmt, ap);
		va_end(ap);
	}

	/* chomp last newline character */
	pg_strip_crlf(msg.data);

	ereport(WARNING,
			(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
			 errmsg_internal("libnuma: %s", msg.data)));

	pfree(msg.data);

	errno = olde;
}

void
numa_error(char *where)
{
	int			olde = errno;

	/* chomp last newline character */
	pg_strip_crlf(where);

	/*
	 * XXX: for now we issue just WARNING, but long-term that might depend on
	 * numa_set_strict() here.
	 */
	elog(WARNING, "libnuma: %s", where);
	errno = olde;
}
#endif							/* FRONTEND */

#else

/* Empty wrappers */
int
pg_numa_init(void)
{
	/* We state that NUMA is not available */
	return -1;
}

int
pg_numa_query_pages(int pid, unsigned long count, void **pages, int *status)
{
	return 0;
}

int
pg_numa_get_max_node(void)
{
	return 0;
}

int
pg_numa_interleave_memptr(void *ptr, size_t sz, pg_numa_bitmask_t *mask)
{
	return 0;
}

pg_numa_bitmask_t *
pg_numa_parse_nodestring(const char *string)
{
	return NULL;
}

void
pg_numa_set_bind_policy(int strict)
{
	return;
}

void
pg_numa_bind(pg_numa_bitmask_t *nodemask)
{
	return;
}

#endif
