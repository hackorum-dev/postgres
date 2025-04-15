/*-------------------------------------------------------------------------
 *
 * pg_crc32c_s390x.c
 *	  Hardware-accelerated CRC-32C variants for Linux on IBM Z & LinuxONE
 *
 * This code was originally written by Hendrik Brueckner
 * <brueckner@linux.ibm.com> for use in the Linux kernel and has been
 * relicensed under the postgresql-license.
 *
 * Use the z/Architecture Vector Extension Facility to accelerate the
 * computing of bitreflected CRC-32C checksums.
 *
 * This CRC-32C implementation algorithm is bitreflected and processes
 * the least-significant bit first (Little-Endian).
 *
 * IDENTIFICATION
 *	  src/port/pg_crc32c_s390x.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"
#include <vecintrin.h>
#include "port/pg_crc32c.h"

#define VX_ALIGNMENT 16L
#define VX_ALIGN_MASK (VX_ALIGNMENT - 1)

typedef unsigned char uv16qi __attribute__((vector_size(16)));
typedef unsigned int uv4si __attribute__((vector_size(16)));
typedef unsigned long long uv2di __attribute__((vector_size(16)));

#ifdef HAVE_GETAUXVAL
#include <sys/auxv.h>
#endif

static inline bool
pg_s390x_vx_available(void)
{
#ifdef HAVE_GETAUXVAL
	return (getauxval(AT_HWCAP) & HWCAP_S390_VX);
#else
	return false;
#endif
}

/*
 * This gets called on the first call. It replaces the function pointer
 * so that subsequent calls are routed directly to the chosen implementation.
 */
static inline pg_crc32c
pg_comp_crc32c_choose(pg_crc32c crc, const void *data, size_t len)
{
	if (pg_s390x_vx_available())
	{
		pg_comp_crc32c = pg_comp_crc32c_s390x;
	}
	else
	{
		pg_comp_crc32c = pg_comp_crc32c_sb8;
	}

	return pg_comp_crc32c(crc, data, len);
}

pg_crc32c	(*pg_comp_crc32c) (pg_crc32c crc, const void *data, size_t len) = pg_comp_crc32c_choose;

static uint32_t
crc32_le_vgfm_16(uint32_t crc, const unsigned char *buf, size_t len)
{
	/*----------
	 * The CRC-32C constant block contains reduction constants to fold and
	 * process particular chunks of the input data stream in parallel.
	 *
	 * For the CRC-32C variants, the constants are precomputed according to
	 * these definitions:
	 *
	 *      R1 = [(x4*128+32 mod P'(x) << 32)]' << 1
	 *      R2 = [(x4*128-32 mod P'(x) << 32)]' << 1
	 *      R3 = [(x128+32 mod P'(x) << 32)]'   << 1
	 *      R4 = [(x128-32 mod P'(x) << 32)]'   << 1
	 *      R5 = [(x64 mod P'(x) << 32)]'       << 1
	 *      R6 = [(x32 mod P'(x) << 32)]'       << 1
	 *
	 *      The bitreflected Barret reduction constant, u', is defined as
	 *      the bit reversal of floor(x**64 / P(x)).
	 *
	 *      where P(x) is the polynomial in the normal domain and the P'(x) is the
	 *      polynomial in the reversed (bitreflected) domain.
	 *
	 * CRC-32C (Castagnoli) polynomials:
	 *
	 *      P(x)  = 0x1EDC6F41
	 *      P'(x) = 0x82F63B78
	 */
	const		uv16qi perm_le2be = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0}; /* BE->LE mask */
	const		uv2di r2r1 = {0x09e4addf8, 0x740eef02}; /* R2, R1 */
	const		uv2di r4r3 = {0x14cd00bd6, 0xf20c0dfe}; /* R4, R3 */
	const		uv2di r5 = {0, 0x0dd45aab8};	/* R5 */
	const		uv2di ru_poly = {0, 0x0dea713f1};	/* u' */
	const		uv2di crc_poly = {0, 0x105ec76f0};	/* P'(x) << 1 */
	uv2di		v0 = {0, 0};
	uv2di		v1 = {0, 0};
	uv2di		v2 = {0, 0};
	uv2di		v3 = {0, 0};
	uv2di		v4 = {0, 0};
	uv16qi		v9 = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

	/*
	 * Load the initial CRC value.
	 *
	 * The CRC value is loaded into the rightmost word of the vector register
	 * and is later XORed with the LSB portion of the loaded input data.
	 */
	v0 = (uv2di) vec_insert(crc, (uv4si) v0, 3);

	if (len >= 64)
	{
		/* Load a 64-byte data chunk and XOR with CRC */
		v1 = vec_perm(((uv2di *) buf)[0], ((uv2di *) buf)[0], perm_le2be);
		v2 = vec_perm(((uv2di *) buf)[1], ((uv2di *) buf)[1], perm_le2be);
		v3 = vec_perm(((uv2di *) buf)[2], ((uv2di *) buf)[2], perm_le2be);
		v4 = vec_perm(((uv2di *) buf)[3], ((uv2di *) buf)[3], perm_le2be);

		v1 ^= v0;
		buf += 64;
		len -= 64;

		while (len >= 64)
		{
			/* Load the next 64-byte data chunk */
			uv16qi		part1 = vec_perm(((uv16qi *) buf)[0], ((uv16qi *) buf)[0], perm_le2be);
			uv16qi		part2 = vec_perm(((uv16qi *) buf)[1], ((uv16qi *) buf)[1], perm_le2be);
			uv16qi		part3 = vec_perm(((uv16qi *) buf)[2], ((uv16qi *) buf)[2], perm_le2be);
			uv16qi		part4 = vec_perm(((uv16qi *) buf)[3], ((uv16qi *) buf)[3], perm_le2be);

			/*
			 * Perform a GF(2) multiplication of the doublewords in V1 with
			 * the R1 and R2 reduction constants in V0.  The intermediate
			 * result is then folded (accumulated) with the next data chunk in
			 * PART1 and stored in V1. Repeat this step for the register
			 * contents in V2, V3, and V4 respectively.
			 */
			v1 = (uv2di) vec_gfmsum_accum_128(r2r1, v1, part1);
			v2 = (uv2di) vec_gfmsum_accum_128(r2r1, v2, part2);
			v3 = (uv2di) vec_gfmsum_accum_128(r2r1, v3, part3);
			v4 = (uv2di) vec_gfmsum_accum_128(r2r1, v4, part4);

			buf += 64;
			len -= 64;
		}

		/*
		 * Fold V1 to V4 into a single 128-bit value in V1.  Multiply V1 with
		 * R3 and R4 and accumulating the next 128-bit chunk until a single
		 * 128-bit value remains.
		 */
		v1 = (uv2di) vec_gfmsum_accum_128(r4r3, v1, (uv16qi) v2);
		v1 = (uv2di) vec_gfmsum_accum_128(r4r3, v1, (uv16qi) v3);
		v1 = (uv2di) vec_gfmsum_accum_128(r4r3, v1, (uv16qi) v4);
	}
	else
	{

		/*
		 * Load a 16-byte data chunk and XOR with CRC
		 */
		v1 = vec_perm(((uv2di *) buf)[0], ((uv2di *) buf)[0], perm_le2be);
		v1 ^= v0;
		buf += 16;
		len -= 16;
	}

	while (len >= 16)
	{
		/* Load next data chunk */
		v2 = vec_perm(*(uv2di *) buf, *(uv2di *) buf, perm_le2be);

		/* Fold next data chunk */
		v1 = (uv2di) vec_gfmsum_accum_128(r4r3, v1, (uv16qi) v2);

		buf += 16;
		len -= 16;
	}

	/*
	 * Set up a vector register for byte shifts.  The shift value must be
	 * loaded in bits 1-4 in byte element 7 of a vector register. Shift by 8
	 * bytes: 0x40 Shift by 4 bytes: 0x20
	 */

	v9 = vec_insert((unsigned char) 0x40, v9, 7);

	/*
	 * Prepare V0 for the next GF(2) multiplication: shift V0 by 8 bytes to
	 * move R4 into the rightmost doubleword and set the leftmost doubleword
	 * to 0x1.
	 */
	v0 = vec_srb(r4r3, (uv2di) v9);
	v0[0] = 1;

	/*
	 * Compute GF(2) product of V1 and V0.  The rightmost doubleword of V1 is
	 * multiplied with R4.  The leftmost doubleword of V1 is multiplied by 0x1
	 * and is then XORed with rightmost product. Implicitly, the intermediate
	 * leftmost product becomes padded
	 */
	v1 = (uv2di) vec_gfmsum_128(v0, v1);

	/*
	 * Now do the final 32-bit fold by multiplying the rightmost word in V1
	 * with R5 and XOR the result with the remaining bits in V1.
	 *
	 * To achieve this by a single VGFMAG, right shift V1 by a word and store
	 * the result in V2 which is then accumulated.  Use the vector unpack
	 * instruction to load the rightmost half of the doubleword into the
	 * rightmost doubleword element of V1; the other half is loaded in the
	 * leftmost doubleword. The vector register with CONST_R5 contains the R5
	 * constant in the rightmost doubleword and the leftmost doubleword is
	 * zero to ignore the leftmost product of V1.
	 */
	v9 = vec_insert((unsigned char) 0x20, v9, 7);
	v2 = vec_srb(v1, (uv2di) v9);
	v1 = vec_unpackl((uv4si) v1);	/* Split rightmost doubleword */
	v1 = (uv2di) vec_gfmsum_accum_128(r5, v1, (uv16qi) v2);

	/*----------
	 * Apply a Barret reduction to compute the final 32-bit CRC value.
	 *
	 * The input values to the Barret reduction are the degree-63 polynomial
	 * in V1 (R(x)), degree-32 generator polynomial, and the reduction
	 * constant u.  The Barret reduction result is the CRC value of R(x) mod
	 * P(x).
	 *
	 * The Barret reduction algorithm is defined as:
	 *
	 *    1. T1(x) = floor( R(x) / x^32 ) GF2MUL u
	 *    2. T2(x) = floor( T1(x) / x^32 ) GF2MUL P(x)
	 *    3. C(x)  = R(x) XOR T2(x) mod x^32
	 *
	 *  Note: The leftmost doubleword of vector register containing
	 *  CONST_RU_POLY is zero and, thus, the intermediate GF(2) product
	 *  is zero and does not contribute to the final result.
	 */

	/* T1(x) = floor( R(x) / x^32 ) GF2MUL u */
	v2 = vec_unpackl((uv4si) v1);
	v2 = (uv2di) vec_gfmsum_128(ru_poly, v2);

	/*
	 * Compute the GF(2) product of the CRC polynomial with T1(x) in V2 and
	 * XOR the intermediate result, T2(x), with the value in V1. The final
	 * result is stored in word element 2 of V2.
	 */
	v2 = vec_unpackl((uv4si) v2);
	v2 = (uv2di) vec_gfmsum_accum_128(crc_poly, v2, (uv16qi) v1);

	return ((uv4si) v2)[2];
}

pg_crc32c
pg_comp_crc32c_s390x(pg_crc32c crc, const void *data, size_t len)
{
	uintptr_t	prealign,
				aligned,
				remaining;
	const unsigned char *buf = data;

	/*
	 * Preprocess initial bytes with sb8 so the hw can start at an aligned
	 * address
	 */
	if ((uintptr_t) buf & VX_ALIGN_MASK)
	{
		prealign = VX_ALIGNMENT - ((uintptr_t) buf & VX_ALIGN_MASK);
		len -= prealign;
		crc = pg_comp_crc32c_sb8(crc, buf, prealign);
		buf += prealign;
	}
	aligned = len & ~VX_ALIGN_MASK;
	remaining = len & VX_ALIGN_MASK;

	/* Process major part of the data with hw acceleration */
	if (aligned)
	{
		crc = pg_bswap32(crc32_le_vgfm_16(pg_bswap32(crc), buf, (size_t) aligned));
	}

	/* Process remaining bytes that could not be handled by hw */
	if (remaining)
	{
		crc = pg_comp_crc32c_sb8(crc, buf + aligned, remaining);
	}

	return crc;
}
