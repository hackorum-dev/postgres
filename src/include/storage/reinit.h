/*-------------------------------------------------------------------------
 *
 * reinit.h
 *	  Reinitialization of unlogged relations
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/reinit.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef REINIT_H
#define REINIT_H

#include "common/relpath.h"


extern void ResetUnloggedRelations(int op);
extern bool parse_filename_for_nontemp_relation(const char *name,
												int *oidchars, ForkNumber *fork);

#define UNLOGGED_RELATION_CLEANUP		(1 << 0)
#define UNLOGGED_RELATION_INIT			(1 << 1)

#endif							/* REINIT_H */
