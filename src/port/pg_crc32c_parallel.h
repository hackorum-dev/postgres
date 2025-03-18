/*-------------------------------------------------------------------------
 *
 * pg_crc32c_parallel.h
 *	  Hardware-independent template for parallel CRC computation.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 *	  src/port/pg_crc32c_parallel.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CRC32C_H
#define PG_CRC32C_H

if (unlikely(len >= MIN_PARALLEL_LENGTH))
{
	/*
	 * Align pointer regardless of architecture to avoid straddling cacheline
	 * boundaries, since we issue three loads per loop iteration below.
	 */
	for (; (uintptr_t) p & 7; len--)
		crc0 = PG_CRC32C_1B(crc0, *p++);

	/*
	 * A CRC instruction can be issued every cycle on many architectures, but
	 * the latency of its result will take several cycles. We can take
	 * advantage of this by dividing the input into 3 equal blocks and
	 * computing the CRC of each independently.
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
			crc0 = PG_CRC32C_8B(crc0, *(in64));
			crc1 = PG_CRC32C_8B(crc1, *(in64 + block_len));
			crc2 = PG_CRC32C_8B(crc2, *(in64 + block_len * 2));
		}

		/*
		 * Combine the partial CRCs using carryless multiplication on
		 * pre-computed length-specific constants.
		 */
		precompute = combine_crc_lookup[block_len - 1];
		mul0 = pg_clmul(crc0, (uint32) precompute);
		mul1 = pg_clmul(crc1, (uint32) (precompute >> 32));
		crc0 = PG_CRC32C_8B(0, mul0);
		crc0 ^= PG_CRC32C_8B(0, mul1);
		crc0 ^= crc2;

		p += block_len * CRC_BYTES_PER_ITER;
		len -= block_len * CRC_BYTES_PER_ITER;
	}
}

#endif							/* PG_CRC32C_H */
