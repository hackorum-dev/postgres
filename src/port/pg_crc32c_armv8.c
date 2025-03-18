/*-------------------------------------------------------------------------
 *
 * pg_crc32c_armv8.c
 *	  Compute CRC-32C checksum using ARMv8 CRC Extension instructions
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_crc32c_armv8.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <arm_acle.h>

#include "port/pg_crc32c.h"

#define DEBUG_CRC				/* XXX not for commit */

static pg_crc32c pg_comp_crc32c_armv8_tail(pg_crc32c crc, const void *data, size_t len);


pg_crc32c
pg_comp_crc32c_armv8(pg_crc32c crc, const void *data, size_t len)
{
	const unsigned char *p = data;
	pg_crc32c	crc0 = crc;

#ifdef DEBUG_CRC
	const size_t orig_len PG_USED_FOR_ASSERTS_ONLY = len;
#endif

/* min size to compute multiple segments in parallel */
#define MIN_PARALLEL_LENGTH 600

#define PG_CRC32C_1B(c, w) __crc32cb(c, w)
#define PG_CRC32C_8B(c, w) __crc32cd(c, w)
#include "pg_crc32c_parallel.h"

	crc0 = pg_comp_crc32c_armv8_tail(crc0, p, len);

#ifdef DEBUG_CRC
	Assert(crc0 == pg_comp_crc32c_sb8(crc, data, orig_len));
#endif

	return crc0;
}

static pg_crc32c
pg_comp_crc32c_armv8_tail(pg_crc32c crc, const void *data, size_t len)
{
	const unsigned char *p = data;
	const unsigned char *pend = p + len;

	/*
	 * ARMv8 doesn't require alignment, but aligned memory access is
	 * significantly faster. Process leading bytes so that the loop below
	 * starts with a pointer aligned to eight bytes.
	 */
	if (!PointerIsAligned(p, uint16) &&
		p + 1 <= pend)
	{
		crc = __crc32cb(crc, *p);
		p += 1;
	}
	if (!PointerIsAligned(p, uint32) &&
		p + 2 <= pend)
	{
		crc = __crc32ch(crc, *(uint16 *) p);
		p += 2;
	}
	if (!PointerIsAligned(p, uint64) &&
		p + 4 <= pend)
	{
		crc = __crc32cw(crc, *(uint32 *) p);
		p += 4;
	}

	/* Process eight bytes at a time, as far as we can. */
	while (p + 8 <= pend)
	{
		crc = __crc32cd(crc, *(uint64 *) p);
		p += 8;
	}

	/* Process remaining 0-7 bytes. */
	if (p + 4 <= pend)
	{
		crc = __crc32cw(crc, *(uint32 *) p);
		p += 4;
	}
	if (p + 2 <= pend)
	{
		crc = __crc32ch(crc, *(uint16 *) p);
		p += 2;
	}
	if (p < pend)
	{
		crc = __crc32cb(crc, *p);
	}

	return crc;
}
