/*-------------------------------------------------------------------------
 *
 * arch-loongarch.h
 *	  Atomic operations considerations specific to LoongArch
 *
 * Portions Copyright (c) 2013-2026, PostgreSQL Global Development Group
 *
 * NOTES:
 *
 * src/include/port/atomics/arch-loongarch.h
 *
 *-------------------------------------------------------------------------
 */

/* intentionally no include guards, should only be included by atomics.h */
#ifndef INSIDE_ATOMICS_H
#error "should be included via atomics.h"
#endif

/*
 * A naturally aligned 8-byte load or store is single-copy atomic on
 * LoongArch64, so the generic 64-bit atomic implementation can use a plain
 * load/store instead of a compare/exchange loop.
 */
#if defined(__loongarch_lp64)
#define PG_HAVE_8BYTE_SINGLE_COPY_ATOMICITY
#endif  /* __loongarch_lp64 */
