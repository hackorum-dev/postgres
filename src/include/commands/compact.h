/*-------------------------------------------------------------------------
 *
 * compact.h
 *	  header file for the COMPACT command
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/compact.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COMPACT_H
#define COMPACT_H

#include "nodes/parsenodes.h"
#include "parser/parse_node.h"

extern void ExecCompact(ParseState *pstate, CompactStmt *stmt, bool isTopLevel);

#endif							/* COMPACT_H */
