/*-------------------------------------------------------------------------
 *
 * execdesc.h
 *	  plan and query descriptor accessor macros used by the executor
 *	  and related modules.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/execdesc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECDESC_H
#define EXECDESC_H

#include "nodes/execnodes.h"
#include "tcop/dest.h"


/* ----------------
 *		query descriptor:
 *
 *	a QueryDesc encapsulates everything that the executor
 *	needs to execute the query.
 *
 *	For the convenience of SQL-language functions, we also support QueryDescs
 *	containing utility statements; these must not be passed to the executor
 *	however.
 * ---------------------
 */
typedef struct QueryDesc
{
	/* These fields are provided by CreateQueryDesc */
	CmdType		operation;		/* CMD_SELECT, CMD_UPDATE, etc. */
	PlannedStmt *plannedstmt;	/* planner's output (could be utility, too) */
	const char *sourceText;		/* source text of the query */
	Snapshot	snapshot;		/* snapshot to use for query */
	Snapshot	crosscheck_snapshot;	/* crosscheck for RI update/delete */
	DestReceiver *dest;			/* the destination for tuple output */
	ParamListInfo params;		/* param values being passed in */
	QueryEnvironment *queryEnv; /* query environment passed in */
	int			instrument_options; /* OR of InstrumentOption flags */
	int			query_instr_options;	/* OR of InstrumentOption flags for
										 * query_instr */

	/*
	 * Nesting level at which this query was started, or -1 if not set.
	 *
	 * A query can end at a different nesting level than it started at.  For
	 * example, a WITH HOLD cursor declared at top level is closed at COMMIT.
	 * An extension that needs the starting level in ExecutorEnd can save it
	 * here from its ExecutorStart hook.
	 *
	 * A statement sent by the client is at level 0, and so is one run by
	 * EXECUTE.  A query started by another statement, while that statement is
	 * being planned, executed or processed as a utility command, is one level
	 * deeper.  For example, the query of a top-level DECLARE CURSOR, EXPLAIN
	 * or CREATE TABLE AS is at level 1, and so is a query run by a function
	 * called from a top-level statement.  This is how, for example,
	 * pg_stat_statements counts levels.
	 *
	 * An extension that counts levels differently must not set this field.
	 */
	int			start_nesting_level;

	/* These fields are set by ExecutorStart */
	TupleDesc	tupDesc;		/* descriptor for result tuples */
	EState	   *estate;			/* executor's query-wide state */
	PlanState  *planstate;		/* tree of per-plan-node state */

	/* This field is set by ExecutePlan */
	bool		already_executed;	/* true if previously executed */

	/* This field is allocated by ExecutorStart if needed */
	struct Instrumentation *query_instr;	/* query level instrumentation */
} QueryDesc;

/* in pquery.c */
extern QueryDesc *CreateQueryDesc(PlannedStmt *plannedstmt,
								  const char *sourceText,
								  Snapshot snapshot,
								  Snapshot crosscheck_snapshot,
								  DestReceiver *dest,
								  ParamListInfo params,
								  QueryEnvironment *queryEnv,
								  int instrument_options);

extern void FreeQueryDesc(QueryDesc *qdesc);

#endif							/* EXECDESC_H  */
