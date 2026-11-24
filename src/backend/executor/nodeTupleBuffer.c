/*-------------------------------------------------------------------------
 *
 * nodeTupleBuffer.c
 *	  Buffer a subplan to completion before returning its first tuple.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeTupleBuffer.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "executor/executor.h"
#include "executor/nodeTupleBuffer.h"
#include "miscadmin.h"
#include "utils/tuplestore.h"

static TupleTableSlot *
ExecTupleBuffer(PlanState *pstate)
{
	TupleBufferState *node = castNode(TupleBufferState, pstate);
	EState	   *estate = pstate->state;
	ScanDirection dir = estate->es_direction;
	TupleTableSlot *slot = pstate->ps_ResultTupleSlot;

	CHECK_FOR_INTERRUPTS();

	if (!node->filled)
	{
		PlanState  *outer = outerPlanState(node);
		bool		enter_parallel_mode = estate->es_parallel_insert_select &&
			castNode(TupleBuffer, pstate->plan)->mode == TUPLEBUFFER_PARALLEL;
		TupleTableSlot *input;

		if (node->tuplestore == NULL)
		{
			node->tuplestore = tuplestore_begin_heap(true, false, work_mem);
			tuplestore_set_eflags(node->tuplestore, node->eflags);
			if (node->eflags & EXEC_FLAG_MARK)
				node->markptr = tuplestore_alloc_read_pointer(node->tuplestore,
															  node->eflags);
		}

		if (enter_parallel_mode)
		{
			EnterParallelMode();
			estate->es_use_parallel_mode = true;
		}

		estate->es_direction = ForwardScanDirection;
		for (;;)
		{
			input = ExecProcNode(outer);
			if (TupIsNull(input))
				break;
			tuplestore_puttupleslot(node->tuplestore, input);
		}
		if (enter_parallel_mode)
		{
			/*
			 * EOF need not have exhausted a Gather below a Limit, including
			 * one in an initplan attached to the buffer itself.  Stop all
			 * such workers before allowing the first write.
			 */
			ExecShutdownNode(outer);
			foreach_node(SubPlanState, sstate, pstate->initPlan)
				ExecShutdownNode(sstate->planstate);
			estate->es_use_parallel_mode = false;
			ExitParallelMode();
		}
		estate->es_direction = dir;
		node->filled = true;
	}

	tuplestore_gettupleslot(node->tuplestore, ScanDirectionIsForward(dir),
							false, slot);
	return slot;
}

static TupleTableSlot *
ExecTupleBufferPassthrough(PlanState *pstate)
{
	CHECK_FOR_INTERRUPTS();
	return ExecProcNode(outerPlanState(pstate));
}

TupleBufferState *
ExecInitTupleBuffer(TupleBuffer *plan, EState *estate, int eflags)
{
	TupleBufferState *node = makeNode(TupleBufferState);

	node->ps.plan = (Plan *) plan;
	node->ps.state = estate;

	node->eflags = eflags & (EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD |
							 EXEC_FLAG_MARK);
	if (plan->mode == TUPLEBUFFER_PASSTHROUGH)
	{
		/* The child supplies the slot and handles all positioning requests. */
		node->ps.ExecProcNode = ExecTupleBufferPassthrough;
		outerPlanState(node) = ExecInitNode(outerPlan(plan), estate, eflags);
		ExecInitResultTypeTL(&node->ps);
		node->ps.resultops = ExecGetResultSlotOps(outerPlanState(node),
												  &node->ps.resultopsfixed);
		node->ps.resultopsset = true;
		return node;
	}

	node->ps.ExecProcNode = ExecTupleBuffer;
	/* A backwards scan must be able to reach the start of the buffer. */
	if (eflags & EXEC_FLAG_BACKWARD)
		node->eflags |= EXEC_FLAG_REWIND;

	/* The child is only ever scanned forwards. */
	outerPlanState(node) = ExecInitNode(outerPlan(plan), estate,
										eflags & ~node->eflags);
	ExecInitResultTupleSlotTL(&node->ps, &TTSOpsMinimalTuple);

	return node;
}

void
ExecReScanTupleBuffer(TupleBufferState *node)
{
	PlanState  *outer = outerPlanState(node);

	if (castNode(TupleBuffer, node->ps.plan)->mode == TUPLEBUFFER_PASSTHROUGH)
	{
		if (outer->chgParam == NULL)
			ExecReScan(outer);
		return;
	}

	ExecClearTuple(node->ps.ps_ResultTupleSlot);
	if (node->tuplestore == NULL)
		return;

	if (outer->chgParam == NULL && (node->eflags & EXEC_FLAG_REWIND))
		tuplestore_rescan(node->tuplestore);
	else
	{
		/* Keep the store's peak usage across fills for EXPLAIN ANALYZE. */
		tuplestore_clear(node->tuplestore);
		node->filled = false;
		if (outer->chgParam == NULL)
			ExecReScan(outer);
	}
}

void
ExecTupleBufferMarkPos(TupleBufferState *node)
{
	Assert(node->eflags & EXEC_FLAG_MARK);
	if (castNode(TupleBuffer, node->ps.plan)->mode == TUPLEBUFFER_PASSTHROUGH)
		ExecMarkPos(outerPlanState(node));
	else if (node->tuplestore != NULL)
	{
		tuplestore_copy_read_pointer(node->tuplestore, 0, node->markptr);
		tuplestore_trim(node->tuplestore);
	}
}

void
ExecTupleBufferRestrPos(TupleBufferState *node)
{
	Assert(node->eflags & EXEC_FLAG_MARK);
	if (castNode(TupleBuffer, node->ps.plan)->mode == TUPLEBUFFER_PASSTHROUGH)
		ExecRestrPos(outerPlanState(node));
	else if (node->tuplestore != NULL)
		tuplestore_copy_read_pointer(node->tuplestore, node->markptr, 0);
}

void
ExecEndTupleBuffer(TupleBufferState *node)
{
	if (node->tuplestore != NULL)
	{
		tuplestore_end(node->tuplestore);
		node->tuplestore = NULL;
	}
	ExecEndNode(outerPlanState(node));
}
