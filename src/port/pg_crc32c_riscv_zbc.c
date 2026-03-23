/*-------------------------------------------------------------------------
 *
 * pg_crc32c_riscv_zbc.c
 *	  Compute CRC-32C checksum using RISC-V Zbc carry-less multiply instructions
 *
 * This implementation uses the RISC-V Zbc (or Zbkc) extension for hardware-
 * accelerated CRC-32C computation. It uses carry-less multiplication (clmul
 * and clmulh) with polynomial folding and Barrett reduction.
 *
 * The algorithm is based on Google Abseil's implementation:
 * https://github.com/abseil/abseil-cpp/pull/1986
 * File: absl/crc/internal/crc_riscv.cc
 *
 * Copyright 2025 The Abseil Authors
 * Licensed under the Apache License, Version 2.0
 * Adapted for PostgreSQL under PostgreSQL license
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_crc32c_riscv_zbc.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#ifdef WORDS_BIGENDIAN
#error "RISC-V Zbc CRC implementation does not support big-endian systems"
#endif

#include "port/pg_crc32c.h"

/*
 * 128-bit value for polynomial arithmetic
 */
typedef struct
{
	uint64		lo;
	uint64		hi;
} V128;

/*
 * Carry-less multiply instructions from RISC-V Zbc/Zbkc extension
 */
static inline uint64
pg_clmul(uint64 a, uint64 b)
{
	uint64		_res;

	__asm__(
			"	clmul %0, %1, %2\n"
:			"=r"(_res)
:			"r"(a), "r"(b));

	return _res;
}

static inline uint64
pg_clmulh(uint64 a, uint64 b)
{
	uint64		_res;

	__asm__(
			"	clmulh %0, %1, %2"
:			"=r"(_res)
:			"r"(a), "r"(b));

	return _res;
}

static inline V128
pg_clmul128(uint64 a, uint64 b)
{
	V128		result;

	result.lo = pg_clmul(a, b);
	result.hi = pg_clmulh(a, b);
	return result;
}

/*
 * 128-bit operations
 */
static inline V128
pg_v128_xor(V128 a, V128 b)
{
	V128		result;

	result.lo = a.lo ^ b.lo;
	result.hi = a.hi ^ b.hi;
	return result;
}

static inline V128
pg_v128_and_mask32(V128 a)
{
	V128		result;

	result.lo = a.lo & UINT64CONST(0x00000000FFFFFFFF);
	result.hi = a.hi & UINT64CONST(0x00000000FFFFFFFF);
	return result;
}

static inline V128
pg_v128_shift_right64(V128 a)
{
	V128		result;

	result.lo = a.hi;
	result.hi = 0;
	return result;
}

static inline V128
pg_v128_shift_right32(V128 a)
{
	V128		result;

	result.lo = (a.lo >> 32) | (a.hi << 32);
	result.hi = (a.hi >> 32);
	return result;
}

static inline V128
pg_v128_load(const unsigned char *p)
{
	V128		result;

	/*
	 * Load 16 bytes as two 64-bit values. Use direct loads like Abseil
	 * reference implementation. RISC-V is always little-endian so no byte
	 * swapping needed.
	 */
	result.lo = *(const uint64 *) p;
	result.hi = *(const uint64 *) (p + 8);
	return result;
}

/*
 * CRC-32C (Castagnoli) polynomial folding constants. These are computed
 * for the polynomial 0x1EDC6F41 (normal form) or 0x82F63B78 (reflected).
 */
static const uint64 kK5 = UINT64CONST(0x0f20c0dfe); /* Folding constant */
static const uint64 kK6 = UINT64CONST(0x14cd00bd6); /* Folding constant */
static const uint64 kK7 = UINT64CONST(0x0dd45aab8); /* 64->32 reduction */
static const uint64 kP1 = UINT64CONST(0x105ec76f0); /* Barrett reduction */
static const uint64 kP2 = UINT64CONST(0x0dea713f1); /* Barrett reduction */

/*
 * Core CRC-32C computation using carry-less multiplication.
 *
 * Input: CRC in working form (already inverted with ~crc)
 * Output: CRC in working form (still inverted)
 *
 * Precondition: len >= 32 and len % 16 == 0
 */
static uint32
pg_crc32c_clmul_core(uint32 crc_inverted, const unsigned char *buf, uint64 len)
{
	V128		x;

	/* Load first 16-byte block and XOR with inverted CRC */
	x = pg_v128_load(buf);
	x.lo ^= (uint64) crc_inverted;
	buf += 16;
	len -= 16;

	/* Fold 16-byte blocks into 128-bit accumulator */
	while (len >= 16)
	{
		V128		block = pg_v128_load(buf);
		V128		lo = pg_clmul128(x.lo, kK5);
		V128		hi = pg_clmul128(x.hi, kK6);

		x = pg_v128_xor(pg_v128_xor(lo, hi), block);
		buf += 16;
		len -= 16;
	}

	/* Reduce 128-bit to 64-bit */
	{
		V128		tmp = pg_clmul128(kK6, x.lo);

		x = pg_v128_xor(pg_v128_shift_right64(x), tmp);
	}

	/* Reduce 64-bit to 32-bit */
	{
		V128		tmp = pg_v128_shift_right32(x);

		x = pg_v128_and_mask32(x);
		x = pg_clmul128(kK7, x.lo);
		x = pg_v128_xor(x, tmp);
	}

	/* Barrett reduction to final 32-bit CRC */
	{
		V128		tmp = pg_v128_and_mask32(x);

		tmp = pg_clmul128(kP2, tmp.lo);
		tmp = pg_v128_and_mask32(tmp);
		tmp = pg_clmul128(kP1, tmp.lo);
		x = pg_v128_xor(x, tmp);
	}

	/* Extract result from second 32-bit lane */
	return (uint32) ((x.lo >> 32) & UINT64CONST(0xFFFFFFFF));
}

/*
 * Main CRC-32C computation function with RISC-V Zbc acceleration
 */
pg_crc32c
pg_comp_crc32c_riscv_zbc(pg_crc32c crc, const void *data, size_t len)
{
	const unsigned char *p = data;
	const size_t kMinLen = 32;
	const size_t kChunkLen = 16;
	size_t		tail;

	/* Use software fallback for small buffers */
	if (len < kMinLen)
		return pg_comp_crc32c_sb8(crc, data, len);

	/*
	 * Process head bytes to align to 16-byte boundary if needed. The hardware
	 * algorithm requires 16-byte aligned access.
	 */
	/* Process tail bytes with software (Abseil approach) */
	tail = len % kChunkLen;
	if (tail)
	{
		crc = pg_comp_crc32c_sb8(crc, p, tail);
		p += tail;
		len -= tail;
	}

	/*
	 * Process remaining bytes (now a multiple of 16) with hardware. The core
	 * algorithm requires at least 32 bytes.
	 */
	if (len >= 32)
	{
		/*
		 * The Abseil core algorithm expects to receive 0xFFFFFFFF as the
		 * initial CRC value (corresponding to Abseil's initial value of 0
		 * after inversion). PostgreSQL's convention already passes 0xFFFFFFFF
		 * initially, so pass it directly. The core returns a value that needs
		 * final XOR with 0xFFFFFFFF (done by the caller).
		 */
		crc = pg_crc32c_clmul_core(crc, p, len);
	}

	return crc;
}
