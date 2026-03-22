/*-------------------------------------------------------------------------
 *
 * pg_popcount_riscv.c
 *	  Holds the RISC-V Zbb popcount implementations.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/pg_popcount_riscv.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#ifdef USE_RISCV_ZBB_WITH_RUNTIME_CHECK

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>

/*
 * Try to pull in <asm/hwprobe.h> for RISCV_HWPROBE_* / struct riscv_hwprobe.
 * On older kernel-headers packages (or non-RISC-V Linux distros configured
 * without multiarch headers) the file may be absent; provide minimal
 * fallback definitions so this file still builds.  The runtime check below
 * will gracefully report "unavailable" if the syscall fails.
 */
#if defined(__has_include)
#if __has_include(<asm/hwprobe.h>)
#include <asm/hwprobe.h>
#define HAVE_ASM_HWPROBE_H 1
#endif
#endif

#ifndef HAVE_ASM_HWPROBE_H
struct riscv_hwprobe
{
	int64		key;
	uint64		value;
};
#define RISCV_HWPROBE_KEY_IMA_EXT_0	4
#define RISCV_HWPROBE_EXT_ZBB		(UINT64CONST(1) << 4)
#endif

#ifndef __NR_riscv_hwprobe
#define __NR_riscv_hwprobe			258
#endif
#endif							/* __linux__ */

#include "port/pg_bitutils.h"

/*
 * Hardware implementation using RISC-V Zbb cpop instruction.
 */
static uint64 pg_popcount_zbb(const char *buf, int bytes);
static uint64 pg_popcount_masked_zbb(const char *buf, int bytes, uint8 mask);

/*
 * The function pointers are initially set to "choose" functions.  These
 * functions will first set the pointers to the right implementations (based on
 * what the current CPU supports) and then will call the pointer to fulfill the
 * caller's request.
 */
static uint64 pg_popcount_choose(const char *buf, int bytes);
static uint64 pg_popcount_masked_choose(const char *buf, int bytes, uint8 mask);
uint64		(*pg_popcount_optimized) (const char *buf, int bytes) = pg_popcount_choose;
uint64		(*pg_popcount_masked_optimized) (const char *buf, int bytes, uint8 mask) = pg_popcount_masked_choose;

static inline bool
pg_popcount_zbb_available(void)
{
#if defined(__linux__)
	struct riscv_hwprobe pair = {.key = RISCV_HWPROBE_KEY_IMA_EXT_0};

	if (syscall(__NR_riscv_hwprobe, &pair, 1, 0, NULL, 0) != 0)
		return false;

	return (pair.value & RISCV_HWPROBE_EXT_ZBB) != 0;
#else
	return false;
#endif
}

static inline void
choose_popcount_functions(void)
{
	if (pg_popcount_zbb_available())
	{
		pg_popcount_optimized = pg_popcount_zbb;
		pg_popcount_masked_optimized = pg_popcount_masked_zbb;
	}
	else
	{
		pg_popcount_optimized = pg_popcount_portable;
		pg_popcount_masked_optimized = pg_popcount_masked_portable;
	}
}

static uint64
pg_popcount_choose(const char *buf, int bytes)
{
	choose_popcount_functions();
	return pg_popcount_optimized(buf, bytes);
}

static uint64
pg_popcount_masked_choose(const char *buf, int bytes, uint8 mask)
{
	choose_popcount_functions();
	return pg_popcount_masked_optimized(buf, bytes, mask);
}

/*
 * pg_popcount64_zbb
 *		Return the number of 1 bits set in word
 *
 * Uses the RISC-V Zbb 'cpop' (count population) instruction via
 * __builtin_popcountll().  When compiled with -march=rv64gc_zbb, GCC and
 * Clang will emit the cpop instruction for this builtin.
 */
static inline int
pg_popcount64_zbb(uint64 word)
{
	return __builtin_popcountll(word);
}

/*
 * pg_popcount_zbb
 *		Returns number of 1 bits in buf
 *
 * Similar approach to x86 SSE4.2 POPCNT: process data in 8-byte chunks using
 * the cpop instruction, with byte-by-byte fallback for remaining data.
 */
static uint64
pg_popcount_zbb(const char *buf, int bytes)
{
	uint64		popcnt = 0;
	const uint64 *words = (const uint64 *) buf;

	/* Process 8-byte chunks */
	while (bytes >= 8)
	{
		popcnt += pg_popcount64_zbb(*words++);
		bytes -= 8;
	}

	buf = (const char *) words;

	/* Process any remaining bytes */
	while (bytes--)
		popcnt += pg_number_of_ones[(unsigned char) *buf++];

	return popcnt;
}

/*
 * pg_popcount_masked_zbb
 *		Returns number of 1 bits in buf after applying the mask to each byte
 */
static uint64
pg_popcount_masked_zbb(const char *buf, int bytes, uint8 mask)
{
	uint64		popcnt = 0;
	uint64		maskv = ~UINT64CONST(0) / 0xFF * mask;
	const uint64 *words = (const uint64 *) buf;

	/* Process 8-byte chunks */
	while (bytes >= 8)
	{
		popcnt += pg_popcount64_zbb(*words++ & maskv);
		bytes -= 8;
	}

	buf = (const char *) words;

	/* Process any remaining bytes */
	while (bytes--)
		popcnt += pg_number_of_ones[(unsigned char) *buf++ & mask];

	return popcnt;
}

#endif							/* USE_RISCV_ZBB_WITH_RUNTIME_CHECK */
