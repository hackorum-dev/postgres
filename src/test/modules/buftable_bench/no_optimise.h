/*-------------------------------------------------------------------------
 *
 * no_optimise.h
 *		Declarations for opaque dummy ops (bodies in no_optimise.c).
 *
 *-------------------------------------------------------------------------
 */
#ifndef NO_OPTIMISE_H
#define NO_OPTIMISE_H

#include "storage/buf_internals.h"

extern void ext_nop(BufferTag *tag, uint64 hashcode);
extern bool ext_BufferTagsEqual(const BufferTag *tag1, const BufferTag *tag2);

#endif							/* NO_OPTIMISE_H */
