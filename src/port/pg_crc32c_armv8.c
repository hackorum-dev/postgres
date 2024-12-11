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

pg_crc32c
pg_comp_crc32c_armv8(pg_crc32c crc, const void *data, size_t len)
{
	const unsigned char *p = data;
	const unsigned char *pend = p + len;
	const size_t min_blocklen = 42; /* Min size to consider interleaving */
	const pg_crc32c orig_crc = crc; // XXX not for commit

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

	/* See pg_crc32c_sse42.c for explanation */
	while (p + min_blocklen * CRC_BYTES_PER_ITER <= pend)
	{
		const size_t block_len = Min(CRC_MAX_BLOCK_LEN, (pend - p) / CRC_BYTES_PER_ITER);
		const uint64 *in64 = (const uint64 *) (p);
		pg_crc32c	crc0 = crc,
					crc1 = 0,
					crc2 = 0;
		uint64		mul0,
					mul1,
					precompute;

		for (int i = 0; i < block_len; i++, in64++)
		{
			crc0 = __crc32cd(crc0, *(in64));
			crc1 = __crc32cd(crc1, *(in64 + block_len));
			crc2 = __crc32cd(crc2, *(in64 + block_len * 2));
		}

		precompute = combine_crc_lookup[block_len - 1];
		mul0 = pg_clmul(crc0, (uint32) precompute);
		mul1 = pg_clmul(crc1, (uint32) (precompute >> 32));

		crc0 = __crc32cd(0, mul0);
		crc1 = __crc32cd(0, mul1);
		crc = crc0 ^ crc1 ^ crc2;

		p += block_len * CRC_BYTES_PER_ITER;
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

	// XXX not for commit
	Assert(crc == pg_comp_crc32c_sb8(orig_crc, data, len));
	return crc;
}
