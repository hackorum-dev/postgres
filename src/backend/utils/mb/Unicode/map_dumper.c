/*-------------------------------------------------------------------------
 *
 *	  Radix map dumper
 *
 * Copyright (c) 2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/Unicode/map_dumper.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mb/pg_wchar.h"

#include "map_dumper.h"

void dump_radix_tree(FILE *out, const pg_mb_radix_tree *rt);

static inline uint32
pg_mb_radix_conv(const pg_mb_radix_tree *rt,
				 int l,
				 unsigned char b1,
				 unsigned char b2,
				 unsigned char b3,
				 unsigned char b4)
{
	if (l == 4)
	{
		/* 4-byte code */

		/* check code validity */
		if (b1 < rt->b4_1_lower || b1 > rt->b4_1_upper ||
			b2 < rt->b4_2_lower || b2 > rt->b4_2_upper ||
			b3 < rt->b4_3_lower || b3 > rt->b4_3_upper ||
			b4 < rt->b4_4_lower || b4 > rt->b4_4_upper)
			return 0;

		/* perform lookup */
		if (rt->chars32)
		{
			uint32		idx = rt->b4root;

			idx = rt->chars32[b1 + idx - rt->b4_1_lower];
			idx = rt->chars32[b2 + idx - rt->b4_2_lower];
			idx = rt->chars32[b3 + idx - rt->b4_3_lower];
			return rt->chars32[b4 + idx - rt->b4_4_lower];
		}
		else
		{
			uint16		idx = rt->b4root;

			idx = rt->chars16[b1 + idx - rt->b4_1_lower];
			idx = rt->chars16[b2 + idx - rt->b4_2_lower];
			idx = rt->chars16[b3 + idx - rt->b4_3_lower];
			return rt->chars16[b4 + idx - rt->b4_4_lower];
		}
	}
	else if (l == 3)
	{
		/* 3-byte code */

		/* check code validity */
		if (b2 < rt->b3_1_lower || b2 > rt->b3_1_upper ||
			b3 < rt->b3_2_lower || b3 > rt->b3_2_upper ||
			b4 < rt->b3_3_lower || b4 > rt->b3_3_upper)
			return 0;

		/* perform lookup */
		if (rt->chars32)
		{
			uint32		idx = rt->b3root;

			idx = rt->chars32[b2 + idx - rt->b3_1_lower];
			idx = rt->chars32[b3 + idx - rt->b3_2_lower];
			return rt->chars32[b4 + idx - rt->b3_3_lower];
		}
		else
		{
			uint16		idx = rt->b3root;

			idx = rt->chars16[b2 + idx - rt->b3_1_lower];
			idx = rt->chars16[b3 + idx - rt->b3_2_lower];
			return rt->chars16[b4 + idx - rt->b3_3_lower];
		}
	}
	else if (l == 2)
	{
		/* 2-byte code */

		/* check code validity - first byte */
		if (b3 < rt->b2_1_lower || b3 > rt->b2_1_upper ||
			b4 < rt->b2_2_lower || b4 > rt->b2_2_upper)
			return 0;

		/* perform lookup */
		if (rt->chars32)
		{
			uint32		idx = rt->b2root;

			idx = rt->chars32[b3 + idx - rt->b2_1_lower];
			return rt->chars32[b4 + idx - rt->b2_2_lower];
		}
		else
		{
			uint16		idx = rt->b2root;

			idx = rt->chars16[b3 + idx - rt->b2_1_lower];
			return rt->chars16[b4 + idx - rt->b2_2_lower];
		}
	}
	else if (l == 1)
	{
		/* 1-byte code */

		/* check code validity - first byte */
		if (b4 < rt->b1_lower || b4 > rt->b1_upper)
			return 0;

		/* perform lookup */
		if (rt->chars32)
			return rt->chars32[b4 + rt->b1root - rt->b1_lower];
		else
			return rt->chars16[b4 + rt->b1root - rt->b1_lower];
	}
	return 0; /* shouldn't happen */
}


static inline void
print_one_line(FILE *out, uint32 f, uint32 t)
{
	fprintf(out, "%x %x\n", f, t);
}

/*
 *  pg_mb_radix_tree: dump the whole radix tree conversion
 */
void
dump_radix_tree(FILE *out, const pg_mb_radix_tree *rt)
{
	unsigned int b1, b2, b3, b4;

	if (rt->b1_lower > 0 || rt->b1_upper > 0)
	{
		for (b4 = rt->b1_lower ; b4 <= rt->b1_upper; b4++)
		{
			uint32 r = pg_mb_radix_conv(rt, 1, 0, 0, 0, b4);
			if (r != 0)
				print_one_line(out, b4, r);
		}
	}

	if (rt->b2_1_lower > 0 || rt->b2_1_upper > 0 ||
		rt->b2_2_lower > 0 || rt->b2_2_upper > 0)
	{
		for (b3 = rt->b2_1_lower ; b3 <= rt->b2_1_upper ; b3++)
		{
			for (b4 = rt->b2_2_lower ; b4 <= rt->b2_2_upper ; b4++)
			{
				uint32 r = pg_mb_radix_conv(rt, 2, 0, 0, b3, b4);
				if (r != 0)
					print_one_line(out, (((uint32)b3) << 8) | b4, r);
			}
		}
	}
	if (rt->b3_1_lower > 0 || rt->b3_1_upper > 0 ||
		rt->b3_2_lower > 0 || rt->b3_2_upper > 0 ||
		rt->b3_3_lower > 0 || rt->b3_3_upper > 0)
	{
		for (b2 = rt->b3_1_lower ; b2 <= rt->b3_1_upper ; b2++)
		{
			for (b3 = rt->b3_2_lower ; b3 <= rt->b3_2_upper ; b3++)
			{
				for (b4 = rt->b3_3_lower ; b4 <= rt->b3_3_upper ; b4++)
				{
					uint32 r = pg_mb_radix_conv(rt, 3, 0, b2, b3, b4);
					if (r != 0)
						print_one_line(out,
							(((uint32)b2) << 16) | (((uint32)b3) << 8) | b4,
							r);
				}
			}
		}
	}
	if (rt->b4_1_lower > 0 || rt->b4_1_upper > 0 ||
		rt->b4_2_lower > 0 || rt->b4_2_upper > 0 ||
		rt->b4_3_lower > 0 || rt->b4_3_upper > 0 ||
		rt->b4_4_lower > 0 || rt->b4_4_upper > 0)
	{
		for (b1 = rt->b4_1_lower ; b1 <= rt->b4_1_upper ; b1++)
		{
			for (b2 = rt->b4_2_lower ; b2 <= rt->b4_2_upper ; b2++)
			{
				for (b3 = rt->b4_3_lower ; b3 <= rt->b4_3_upper ; b3++)
				{
					for (b4 = rt->b4_4_lower ; b4 <= rt->b4_4_upper ; b4++)
					{
						uint32 r = pg_mb_radix_conv(rt, 4, b1, b2, b3, b4);
						if (r != 0)
							print_one_line(out,
								(((uint32)b1) << 24) | (((uint32)b2) << 16) |
								(((uint32)b3) << 8) | b4,
								r);
					}
				}
			}
		}
	}
}

int
main(void)
{
	struct mappair *mappair;

	for (mappair = &mappairs[0] ; mappair->name ; mappair++)
	{
		char fname[32];
		FILE *out;

		snprintf(fname, 32, "%s.dump", mappair->name);
		out = fopen(fname, "w");
		if (out == NULL)
		{
			fprintf(stderr, "failed to open dump output file: %s\n", fname);
			exit(1);
		}

		dump_radix_tree(out, mappair->rt);

		fclose(out);
	}

	return 0;
}
