/*-------------------------------------------------------------------------
 *
 * pg_crc32c_riscv_choose.c
 *	  Choose between RISC-V Zbc and software CRC-32C implementation.
 *
 * On first call, checks if the CPU supports the RISC-V Zbc (or Zbkc) extension.
 * If it does, use carry-less multiply instructions for CRC-32C computation.
 * Otherwise, fall back to the pure software implementation (slicing-by-8).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_crc32c_riscv_choose.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <sys/syscall.h>
#include <unistd.h>

#include "port/pg_crc32c.h"

/*
 * RISC-V hardware probing definitions
 */
#ifndef __NR_riscv_hwprobe
#define __NR_riscv_hwprobe 258
#endif

#ifndef RISCV_HWPROBE_KEY_IMA_EXT_0
#define RISCV_HWPROBE_KEY_IMA_EXT_0 4
#endif

#ifndef RISCV_HWPROBE_EXT_ZBC
#define RISCV_HWPROBE_EXT_ZBC (1ULL << 7)
#endif

#ifndef RISCV_HWPROBE_EXT_ZBKC
#define RISCV_HWPROBE_EXT_ZBKC (1ULL << 27)
#endif

struct riscv_hwprobe
{
	int64		key;
	uint64		value;
};

/*
 * Check if RISC-V Zbc or Zbkc extension is available
 *
 * Uses the riscv_hwprobe syscall which is available on Linux kernel 6.4+
 * Falls back to software if the syscall fails or extensions are not available.
 */
static bool
pg_crc32c_riscv_zbc_available(void)
{
#if defined(__linux__) && defined(__riscv) && (__riscv_xlen == 64)
	struct riscv_hwprobe pair = {.key = RISCV_HWPROBE_KEY_IMA_EXT_0};

	/*
	 * Make the syscall. If it fails (e.g., old kernel, non-Linux), fall back
	 * to software.
	 */
	if (syscall(__NR_riscv_hwprobe, &pair, 1, 0, NULL, 0) != 0)
		return false;

	/*
	 * Check if either Zbc (general bitmanip carry-less) or Zbkc (crypto
	 * carry-less) is available. Both provide clmul/clmulh instructions.
	 */
	return (pair.value & (RISCV_HWPROBE_EXT_ZBC | RISCV_HWPROBE_EXT_ZBKC)) != 0;
#else
	/* Not on RISC-V Linux, or not 64-bit - use software fallback */
	return false;
#endif
}

/*
 * This gets called on the first call. It replaces the function pointer
 * so that subsequent calls are routed directly to the chosen implementation.
 */
static pg_crc32c
pg_comp_crc32c_choose(pg_crc32c crc, const void *data, size_t len)
{
	if (pg_crc32c_riscv_zbc_available())
		pg_comp_crc32c = pg_comp_crc32c_riscv_zbc;
	else
		pg_comp_crc32c = pg_comp_crc32c_sb8;

	return pg_comp_crc32c(crc, data, len);
}

pg_crc32c	(*pg_comp_crc32c) (pg_crc32c crc, const void *data, size_t len) = pg_comp_crc32c_choose;
