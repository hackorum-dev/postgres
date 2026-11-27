/*-------------------------------------------------------------------------
 *
 * gb18030.h
 *	  Helpers for GB18030 conversions.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/gb18030.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GB18030_H
#define GB18030_H

/*
 * Convert a 4-byte GB18030 character to an offset in a linear code space.
 *
 * The first and third bytes can range from 0x81 to 0xfe (126 values),
 * while the second and fourth bytes can range from 0x30 to 0x39 (10 values).
 */
static inline uint32
gb_linear(uint32 gb)
{
	uint32		b0 = (gb & 0xff000000) >> 24;
	uint32		b1 = (gb & 0x00ff0000) >> 16;
	uint32		b2 = (gb & 0x0000ff00) >> 8;
	uint32		b3 = (gb & 0x000000ff);

	return b0 * 12600 + b1 * 1260 + b2 * 10 + b3 -
		(0x81 * 12600 + 0x30 * 1260 + 0x81 * 10 + 0x30);
}

#endif
