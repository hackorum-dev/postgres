/*-------------------------------------------------------------------------
 *
 * explain_progressive.h
 *	  prototypes for explain_progressive.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/commands/explain_progressive.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_PROGRESSIVE_H
#define EXPLAIN_PROGRESSIVE_H

#include "datatype/timestamp.h"
#include "executor/executor.h"

typedef struct ProgressiveExplainData
{
	int			plan_alloc_size;
	TimestampTz last_update;
	char		plan[FLEXIBLE_ARRAY_MEMBER];
} ProgressiveExplainData;

extern bool ProgressiveExplainIsActive(QueryDesc *queryDesc);
extern void ProgressiveExplainSetup(QueryDesc *queryDesc);
extern void ProgressiveExplainStart(QueryDesc *queryDesc);
extern void ProgressiveExplainTrigger(void);
extern void ProgressiveExplainUpdate(PlanState *node);
extern void ProgressiveExplainFinish(QueryDesc *queryDesc);
extern Size ProgressiveExplainShmemSize(void);
extern void ProgressiveExplainShmemInit(void);
extern TupleTableSlot *ExecProcNodeInstrExplain(PlanState *node);

/* transaction cleanup code */
extern void AtEOXact_ProgressiveExplain(bool isCommit);
extern void AtEOSubXact_ProgressiveExplain(bool isCommit, int nestDepth);

extern bool ProgressiveExplainPending;

#endif							/* EXPLAIN_PROGRESSIVE_H */
