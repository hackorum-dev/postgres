/*-------------------------------------------------------------------------
 *
 * pg_crc32c_sse42.c
 *	  Compute CRC-32C checksum using Intel SSE 4.2 instructions.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_crc32c_sse42.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <nmmintrin.h>

#include "port/pg_crc32c.h"

/* min size to compute multiple segments in parallel */
#define MIN_PARALLEL_LENGTH 600

static pg_crc32c pg_comp_crc32c_sse42_tail(pg_crc32c crc, const void *data, size_t len);


pg_attribute_target("sse4.2")
pg_crc32c
pg_comp_crc32c_sse42(pg_crc32c crc, const void *data, size_t len)
{
	const unsigned char *p = data;
	pg_crc32c	crc0 = crc;

	/* XXX not for commit */
	const size_t orig_len PG_USED_FOR_ASSERTS_ONLY = len;

#if SIZEOF_VOID_P >= 8
	if (unlikely(len >= MIN_PARALLEL_LENGTH))
	{
		/*
		 * Align pointer to avoid straddling cacheline boundaries, since we
		 * issue three loads per loop iteration below.
		 */
		for (; (uintptr_t) p & 7; len--)
			crc0 = _mm_crc32_u8(crc0, *p++);

		/*
		 * A CRC instruction can be issued every cycle but the latency of its
		 * result will take several cycles. We can take advantage of this by
		 * dividing the input into 3 equal blocks and computing the CRC of
		 * each independently.
		 */
		while (len >= MIN_PARALLEL_LENGTH)
		{
			const size_t block_len = Min(CRC_MAX_BLOCK_LEN,
										 len / CRC_BYTES_PER_ITER);
			const uint64 *in64 = (const uint64 *) (p);
			pg_crc32c	crc1 = 0,
						crc2 = 0;
			uint64		mul0,
						mul1,
						precompute;

			for (int i = 0; i < block_len; i++, in64++)
			{
				crc0 = _mm_crc32_u64(crc0, *(in64));
				crc1 = _mm_crc32_u64(crc1, *(in64 + block_len));
				crc2 = _mm_crc32_u64(crc2, *(in64 + block_len * 2));
			}

			/*
			 * Combine the partial CRCs using carryless multiplication on
			 * pre-computed length-specific constants.
			 */
			precompute = combine_crc_lookup[block_len - 1];
			mul0 = pg_clmul(crc0, (uint32) precompute);
			mul1 = pg_clmul(crc1, (uint32) (precompute >> 32));
			crc0 = _mm_crc32_u64(0, mul0);
			crc0 ^= _mm_crc32_u64(0, mul1);
			crc0 ^= crc2;

			p += block_len * CRC_BYTES_PER_ITER;
			len -= block_len * CRC_BYTES_PER_ITER;
		}
	}
#endif

	crc0 = pg_comp_crc32c_sse42_tail(crc0, p, len);

	/* XXX not for commit */
	Assert(crc0 == pg_comp_crc32c_sb8(crc, data, orig_len));

	return crc0;
}

pg_attribute_no_sanitize_alignment()
pg_attribute_target("sse4.2")
static pg_crc32c
pg_comp_crc32c_sse42_tail(pg_crc32c crc, const void *data, size_t len)
{
	const unsigned char *p = data;
	const unsigned char *pend = p + len;

	/*
	 * Process eight bytes of data at a time.
	 *
	 * NB: We do unaligned accesses here. The Intel architecture allows that,
	 * and performance testing didn't show any performance gain from aligning
	 * the begin address.
	 */
#ifdef __x86_64__
	while (p + 8 <= pend)
	{
		crc = (uint32) _mm_crc32_u64(crc, *((const uint64 *) p));
		p += 8;
	}

	/* Process remaining full four bytes if any */
	if (p + 4 <= pend)
	{
		crc = _mm_crc32_u32(crc, *((const unsigned int *) p));
		p += 4;
	}
#else

	/*
	 * Process four bytes at a time. (The eight byte instruction is not
	 * available on the 32-bit x86 architecture).
	 */
	while (p + 4 <= pend)
	{
		crc = _mm_crc32_u32(crc, *((const unsigned int *) p));
		p += 4;
	}
#endif							/* __x86_64__ */

	/* Process any remaining bytes one at a time. */
	while (p < pend)
	{
		crc = _mm_crc32_u8(crc, *p);
		p++;
	}

	return crc;
}
