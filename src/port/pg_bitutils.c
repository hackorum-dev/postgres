/*-------------------------------------------------------------------------
 *
 * pg_bitutils.c
 *	  Miscellaneous functions for bit-wise operations.
 *
 * Copyright (c) 2019-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/pg_bitutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#ifdef HAVE__GET_CPUID
#include <cpuid.h>
#endif
#ifdef HAVE__CPUID
#include <intrin.h>
#endif

#include "port/pg_bitutils.h"


/*
 * Array giving the position of the left-most set bit for each possible
 * byte value.  We count the right-most position as the 0th bit, and the
 * left-most the 7th bit.  The 0th entry of the array should not be used.
 *
 * Note: this is not used by the functions in pg_bitutils.h when
 * HAVE__BUILTIN_CLZ is defined, but we provide it anyway, so that
 * extensions possibly compiled with a different compiler can use it.
 */
const uint8 pg_leftmost_one_pos[256] = {
	0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
	5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7
};

/*
 * Array giving the position of the right-most set bit for each possible
 * byte value.  We count the right-most position as the 0th bit, and the
 * left-most the 7th bit.  The 0th entry of the array should not be used.
 *
 * Note: this is not used by the functions in pg_bitutils.h when
 * HAVE__BUILTIN_CTZ is defined, but we provide it anyway, so that
 * extensions possibly compiled with a different compiler can use it.
 */
const uint8 pg_rightmost_one_pos[256] = {
	0, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	7, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0
};

/*
 * Array giving the number of 1-bits in each possible byte value.
 *
 * Note: we export this for use by functions in which explicit use
 * of the popcount functions seems unlikely to be a win.
 */
const uint8 pg_number_of_ones[256] = {
	0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
	1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
	1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
	1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
	3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
	4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8
};

/*
 * On x86_64, we can use the hardware popcount instruction, but only if
 * we can verify that the CPU supports it via the cpuid instruction.
 *
 * Otherwise, we fall back to __builtin_popcount if the compiler has that,
 * or a hand-rolled implementation if not.
 */
#ifdef HAVE_X86_64_POPCNTQ
#if defined(HAVE__GET_CPUID) || defined(HAVE__CPUID)
#define USE_POPCNT_ASM 1
#endif
#endif

#ifdef USE_POPCNT_ASM

/*
 * Return true if CPUID indicates that the POPCNT instruction is available.
 */
static bool
pg_popcount_available(void)
{
	unsigned int exx[4] = {0, 0, 0, 0};

#if defined(HAVE__GET_CPUID)
	__get_cpuid(1, &exx[0], &exx[1], &exx[2], &exx[3]);
#elif defined(HAVE__CPUID)
	__cpuid(exx, 1);
#else
#error cpuid instruction not available
#endif

	return (exx[2] & (1 << 23)) != 0;	/* POPCNT */
}

#endif							/* USE_POPCNT_ASM */

static uint64	pg_popcount_slow(const char *buf, int bytes);

#ifdef USE_POPCNT_ASM
static bool pg_popcount_available(void);
static uint64	pg_popcount_choose(const char *buf, int bytes);
static uint64	pg_popcount_asm(const char *buf, int bytes);

uint64 (*pg_popcount) (const char *buf, int bytes) = pg_popcount_choose;
#else
uint64 (*pg_popcount) (const char *buf, int bytes) = pg_popcount_slow;
#endif							/* USE_POPCNT_ASM */


/*
 * pg_popcount32
 *		Return the number of 1 bits set in word
 */
int
pg_popcount32(uint32 word)
{
#ifdef HAVE__BUILTIN_POPCOUNT
	return __builtin_popcount(word);
#else							/* !HAVE__BUILTIN_POPCOUNT */
	int			result = 0;

	while (word != 0)
	{
		result += pg_number_of_ones[word & 255];
		word >>= 8;
	}

	return result;
#endif							/* HAVE__BUILTIN_POPCOUNT */
}

/*
 * pg_popcount64
 *		Return the number of 1 bits set in word
 */
int
pg_popcount64(uint64 word)
{
#ifdef HAVE__BUILTIN_POPCOUNT
#if defined(HAVE_LONG_INT_64)
	return __builtin_popcountl(word);
#elif defined(HAVE_LONG_LONG_INT_64)
	return __builtin_popcountll(word);
#else
#error must have a working 64-bit integer datatype
#endif
#else							/* !HAVE__BUILTIN_POPCOUNT */
	int			result = 0;

	while (word != 0)
	{
		result += pg_number_of_ones[word & 255];
		word >>= 8;
	}

	return result;
#endif							/* HAVE__BUILTIN_POPCOUNT */
}

/*
 * This function gets called on the first call to pg_popcount.
 * It detects whether we can use the asm implementation, and replace
 * the function pointer so that subsequent calls are routed directly to
 * the chosen implementation.
 */
static uint64
pg_popcount_choose(const char *buf, int bytes)
{
	if (pg_popcount_available())
		pg_popcount = pg_popcount_asm;
	else
		pg_popcount = pg_popcount_slow;

	return pg_popcount(buf, bytes);
}


/*
 * pg_popcount_slow
 *		Returns the number of 1-bits in buf
 */
uint64
pg_popcount_slow(const char *buf, int bytes)
{
	uint64		popcnt = 0;

#if SIZEOF_VOID_P >= 8
	/* Process in 64-bit chunks if the buffer is aligned. */
	if (buf == (const char *) TYPEALIGN(8, buf))
	{
		const uint64 *words = (const uint64 *) buf;

		while (bytes >= 8)
		{
			popcnt += pg_popcount64(*words++);
			bytes -= 8;
		}

		buf = (const char *) words;
	}
#else
	/* Process in 32-bit chunks if the buffer is aligned. */
	if (buf == (const char *) TYPEALIGN(4, buf))
	{
		const uint32 *words = (const uint32 *) buf;

		while (bytes >= 4)
		{
			popcnt += pg_popcount32(*words++);
			bytes -= 4;
		}

		buf = (const char *) words;
	}
#endif

	/* Process any remaining bytes */
	while (bytes--)
		popcnt += pg_number_of_ones[(unsigned char) *buf++];

	return popcnt;
}

#ifdef USE_POPCNT_ASM

/*
 * pg_popcount_asm
 *		Returns the number of 1-bits in buf using POPCNT
 */
uint64
pg_popcount_asm(const char *buf, int bytes)
{
	uint64		popcnt = 0;

#if SIZEOF_VOID_P >= 8
	/* Process in 64-bit chunks if the buffer is aligned. */
	if (buf == (const char *) TYPEALIGN(8, buf))
	{
		const uint64 *words = (const uint64 *) buf;
		uint64		cum[4] = {0};

		/*
		 * Manually unroll the loop, since the compiler won't do it for us.
		 * This is in hand-written assembly to prevent some compilers from
		 * inserting useless instructions to work around a False Data
		 * Dependency in the POPCNT instruction.
		 *
		 * Based on code found at http://danluu.com/assembly-intrinsics/
		 */
		while (bytes >= 4 * 8)
		{
__asm__ __volatile__(
			"popcnt %4, %4  \n\t"
			"add %4, %0     \n\t"
			"popcnt %5, %5  \n\t"
			"add %5, %1     \n\t"
			"popcnt %6, %6  \n\t"
			"add %6, %2     \n\t"
			"popcnt %7, %7  \n\t"
			"add %7, %3     \n\t"
			: "+r"  (cum[0]), "+r"  (cum[1]), "+r"  (cum[2]), "+r"  (cum[3])
			: "r" (words[0]), "r" (words[1]), "r" (words[2]), "r" (words[3]));

			bytes -= 4 * 8;
			words += 4;
		}

		popcnt += cum[0] + cum[1] + cum[2] + cum[3];

		while (bytes >= 8)
		{
			popcnt += pg_popcount64(*words++);
			bytes -= 8;
		}

		buf = (const char *) words;
	}
#else
	/* Process in 32-bit chunks if the buffer is aligned. */
	if (buf == (const char *) TYPEALIGN(4, buf))
	{
		const uint32 *words = (const uint32 *) buf;

		while (bytes >= 4)
		{
			popcnt += pg_popcount32(*words++);
			bytes -= 4;
		}

		buf = (const char *) words;
	}
#endif

	/* Process any remaining bytes */
	while (bytes--)
		popcnt += pg_number_of_ones[(unsigned char) *buf++];

	return popcnt;
}

#endif							/* USE_POPCNT_ASM */

/*
 * pg_popcount_mask64
 *		Returns the number of 1-bits in buf after applying the mask
 *
 * Note: We currently only need a 64-bit version for the visibility map.
 */
uint64
pg_popcount_mask64(const uint64 *buf, int bytes, uint64 mask)
{
	uint64		popcnt = 0;

	Assert(bytes % sizeof(uint64) == 0);

	while (bytes >= 8)
	{
		popcnt += pg_popcount64(*words++ & mask);
		bytes -= 8;
	}

	return popcnt;
}
