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

#ifdef USE_SVE2_CRC32C_WITH_RUNTIME_CHECK
#include <arm_sve.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint64_t k_8way_fold[2] __attribute__((aligned(16))) = {
	0x000000006992cea2ULL,		/* x^1055 mod P(x) */
	0x000000000d3b6092ULL		/* x^991 mod P(x) */
};

static const uint64_t k1k2_merge1[2] __attribute__((aligned(16))) = {
	0x00000000f20c0dfeULL,		/* x^159 */
	0x00000000493c7d27ULL		/* x^95 */
};

static const uint64_t k1k2_merge2[2] __attribute__((aligned(16))) = {
	0x000000003da6d0cbULL,		/* x^287 */
	0x00000000ba4fc28eULL		/* x^223 */
};

static const uint64_t k3k4_merge3[2] __attribute__((aligned(16))) = {
	0x00000000740eef02ULL,		/* x^543 */
	0x000000009e4addf8ULL		/* x^479 */
};

pg_attribute_target("arch=armv9-a+sve2-aes")
static inline svuint64_t
sve_load_crc(uint32_t crc)
{
	uint64_t	buf[4] = {(uint64_t) crc, 0};

	return svld1_u64(svptrue_b64(), buf);
}

pg_attribute_target("arch=armv9-a+sve2-aes")
pg_crc32c
pg_comp_crc32c_sve2(pg_crc32c crc_in, const void *data, size_t length)
{
	const uint8_t *input = (const uint8_t *) data;
	pg_crc32c	crc = crc_in;
	svbool_t	pg = svptrue_b64();
	size_t		vl_bytes = (size_t) svcntb();
	svuint64_t	vK_fold = svld1_u64(pg, k_8way_fold);
	svuint64_t	vK_merge1 = svld1_u64(pg, k1k2_merge1);
	svuint64_t	vK_merge2 = svld1_u64(pg, k1k2_merge2);
	svuint64_t	vK_merge3 = svld1_u64(pg, k3k4_merge3);

	if (length < 8 * vl_bytes)
		return pg_comp_crc32c_armv8(crc, input, length);

	/* Initialize 8 accumulators */
	svuint64_t	acc0 = svld1_u64(pg, (const uint64_t *) input);

	acc0 = sveor_u64_z(pg, acc0, sve_load_crc(crc));

	svuint64_t	acc1 = svld1_u64(pg, (const uint64_t *) (input + 1 * vl_bytes));
	svuint64_t	acc2 = svld1_u64(pg, (const uint64_t *) (input + 2 * vl_bytes));
	svuint64_t	acc3 = svld1_u64(pg, (const uint64_t *) (input + 3 * vl_bytes));
	svuint64_t	acc4 = svld1_u64(pg, (const uint64_t *) (input + 4 * vl_bytes));
	svuint64_t	acc5 = svld1_u64(pg, (const uint64_t *) (input + 5 * vl_bytes));
	svuint64_t	acc6 = svld1_u64(pg, (const uint64_t *) (input + 6 * vl_bytes));
	svuint64_t	acc7 = svld1_u64(pg, (const uint64_t *) (input + 7 * vl_bytes));

	input += 8 * vl_bytes;
	length -= 8 * vl_bytes;

	/* Main loop */
	while (length >= 8 * vl_bytes)
	{
		svuint64_t	block0 = svld1_u64(pg, (const uint64_t *) (input + 0 * vl_bytes));
		svuint64_t	block1 = svld1_u64(pg, (const uint64_t *) (input + 1 * vl_bytes));
		svuint64_t	block2 = svld1_u64(pg, (const uint64_t *) (input + 2 * vl_bytes));
		svuint64_t	block3 = svld1_u64(pg, (const uint64_t *) (input + 3 * vl_bytes));
		svuint64_t	block4 = svld1_u64(pg, (const uint64_t *) (input + 4 * vl_bytes));
		svuint64_t	block5 = svld1_u64(pg, (const uint64_t *) (input + 5 * vl_bytes));
		svuint64_t	block6 = svld1_u64(pg, (const uint64_t *) (input + 6 * vl_bytes));
		svuint64_t	block7 = svld1_u64(pg, (const uint64_t *) (input + 7 * vl_bytes));

		svuint64_t	t0_0 = svpmullb_pair_u64(acc0, vK_fold);
		svuint64_t	t0_1 = svpmullb_pair_u64(acc1, vK_fold);
		svuint64_t	t0_2 = svpmullb_pair_u64(acc2, vK_fold);
		svuint64_t	t0_3 = svpmullb_pair_u64(acc3, vK_fold);
		svuint64_t	t0_4 = svpmullb_pair_u64(acc4, vK_fold);
		svuint64_t	t0_5 = svpmullb_pair_u64(acc5, vK_fold);
		svuint64_t	t0_6 = svpmullb_pair_u64(acc6, vK_fold);
		svuint64_t	t0_7 = svpmullb_pair_u64(acc7, vK_fold);

		svuint64_t	t1_0 = svpmullt_pair_u64(acc0, vK_fold);
		svuint64_t	t1_1 = svpmullt_pair_u64(acc1, vK_fold);
		svuint64_t	t1_2 = svpmullt_pair_u64(acc2, vK_fold);
		svuint64_t	t1_3 = svpmullt_pair_u64(acc3, vK_fold);
		svuint64_t	t1_4 = svpmullt_pair_u64(acc4, vK_fold);
		svuint64_t	t1_5 = svpmullt_pair_u64(acc5, vK_fold);
		svuint64_t	t1_6 = svpmullt_pair_u64(acc6, vK_fold);
		svuint64_t	t1_7 = svpmullt_pair_u64(acc7, vK_fold);

		acc0 = sveor3_u64(t0_0, t1_0, block0);
		acc1 = sveor3_u64(t0_1, t1_1, block1);
		acc2 = sveor3_u64(t0_2, t1_2, block2);
		acc3 = sveor3_u64(t0_3, t1_3, block3);
		acc4 = sveor3_u64(t0_4, t1_4, block4);
		acc5 = sveor3_u64(t0_5, t1_5, block5);
		acc6 = sveor3_u64(t0_6, t1_6, block6);
		acc7 = sveor3_u64(t0_7, t1_7, block7);

		input += 8 * vl_bytes;
		length -= 8 * vl_bytes;
	}

	/* Stage 1: Fold 128 bits */
	{
		svuint64_t	t0,
					t1;

		t0 = svpmullb_pair_u64(acc0, vK_merge1);
		t1 = svpmullt_pair_u64(acc0, vK_merge1);
		acc0 = sveor3_u64(t0, t1, acc1);

		t0 = svpmullb_pair_u64(acc2, vK_merge1);
		t1 = svpmullt_pair_u64(acc2, vK_merge1);
		acc2 = sveor3_u64(t0, t1, acc3);

		t0 = svpmullb_pair_u64(acc4, vK_merge1);
		t1 = svpmullt_pair_u64(acc4, vK_merge1);
		acc4 = sveor3_u64(t0, t1, acc5);

		t0 = svpmullb_pair_u64(acc6, vK_merge1);
		t1 = svpmullt_pair_u64(acc6, vK_merge1);
		acc6 = sveor3_u64(t0, t1, acc7);
	}

	/* Stage 2: Fold 256 bits */
	{
		svuint64_t	t0,
					t1;

		t0 = svpmullb_pair_u64(acc0, vK_merge2);
		t1 = svpmullt_pair_u64(acc0, vK_merge2);
		acc0 = sveor3_u64(t0, t1, acc2);

		t0 = svpmullb_pair_u64(acc4, vK_merge2);
		t1 = svpmullt_pair_u64(acc4, vK_merge2);
		acc4 = sveor3_u64(t0, t1, acc6);
	}

	/* Stage 3: Fold 512 bits */
	{
		svuint64_t	t0,
					t1;

		t0 = svpmullb_pair_u64(acc0, vK_merge3);
		t1 = svpmullt_pair_u64(acc0, vK_merge3);
		acc0 = sveor3_u64(t0, t1, acc4);
	}

	/* Remainder loop */
	while (length >= vl_bytes)
	{
		svuint64_t	block = svld1_u64(pg, (const uint64_t *) input);

		svuint64_t	t0 = svpmullb_pair_u64(acc0, vK_merge1);
		svuint64_t	t1 = svpmullt_pair_u64(acc0, vK_merge1);

		acc0 = sveor3_u64(t0, t1, block);

		input += vl_bytes;
		length -= vl_bytes;
	}

	/* Final reduction */
	{
		uint64_t	temp[2];

		svst1_u64(pg, temp, acc0);

		crc = 0;
		crc = __crc32cd(crc, temp[0]);
		crc = __crc32cd(crc, temp[1]);
	}

	if (length > 0)
		crc = pg_comp_crc32c_armv8(crc, input, length);

	return crc;
}
#endif