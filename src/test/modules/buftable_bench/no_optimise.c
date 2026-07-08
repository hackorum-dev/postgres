/*-------------------------------------------------------------------------
 *
 * no_optimise.c
 *		Opaque call targets for the buftable_bench dummy ops.
 *
 * These live in a separate translation unit so the compiler compiling
 * buftable_bench.c cannot see the bodies, inline them, or delete unused
 * calls.  (Does not survive -flto.)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "no_optimise.h"

void
ext_nop(BufferTag *tag, uint64 hashcode)
{
	(void) tag;
	(void) hashcode;
}

bool
ext_BufferTagsEqual(const BufferTag *tag1, const BufferTag *tag2)
{
	return BufferTagsEqual(tag1, tag2);
}
