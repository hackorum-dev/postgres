/*-------------------------------------------------------------------------
 *
 * execScanBatch.h
 *	  Internal support for evaluating scan qualifications in tuple batches.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/executor/execScanBatch.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECSCANBATCH_H
#define EXECSCANBATCH_H

#include "executor/execScan.h"
#include "executor/tuptable.h"

struct ExecScanBatchQual
{
	FmgrInfo	func;
	FunctionCallInfo fcinfo;
	ExprState  *param_expr;
	NullableDatum constant;
	AttrNumber	attnum;
	uint8		var_argno;
};

struct ExecScanBatchState
{
	struct ExecScanBatchQual *quals;
	ExprState  *prefix_qual;
	ExprState  *rest_qual;
	MemoryContext operator_context;
	const TupleTableSlotBatch *batch;
	uint64		generation;
	TupleBatchMask mask;		/* bit i is for batch index first + i */
	int			first;
	int			nrows;
	int			nquals;
};

extern void ExecScanBatchPrepare(ExecScanBatchState *state,
								 TupleTableSlot *slot,
								 ScanDirection direction,
								 ExprContext *econtext);

static pg_always_inline bool
ExecScanBatchReuse(ExecScanBatchState *state, TupleTableSlot *slot,
				   ScanDirection direction, bool *qual_result)
{
	const TupleTableSlotBatch *batch;
	int			position;

	Assert(ScanDirectionIsValid(direction));
	if (state->nrows == 0)
		return false;

	batch = slot_getbatch(slot);
	if (ScanDirectionIsNoMovement(direction) ||
		batch == NULL || state->batch != batch ||
		state->generation != batch->generation)
		goto invalid;

	Assert(state->nrows <= (int) TUPLE_BATCH_MASK_BITS);
	Assert(state->first >= 0);
	Assert(state->nrows <= batch->ntuples);
	Assert(state->first <= batch->ntuples - state->nrows);

	position = batch->current - state->first;
	if (position < 0 || position >= state->nrows)
		goto invalid;

	*qual_result = (state->mask & (UINT64CONST(1) << position)) != 0;
	if (position == (ScanDirectionIsForward(direction) ?
					 state->nrows - 1 : 0))
		ExecResetScanBatch(state);
	return true;

invalid:
	ExecResetScanBatch(state);
	return false;
}

/*
 * Keep the scan loop and expression-context cleanup below in sync with
 * ExecScanExtended(). Differences should remain limited to batch
 * qualification reuse and preparation.
 */
static pg_always_inline TupleTableSlot *
ExecScanBatchExtended(ScanState *node,
					  ExecScanAccessMtd accessMtd,
					  ExecScanRecheckMtd recheckMtd,
					  ExecScanBatchState *state,
					  ProjectionInfo *projInfo)
{
	PlanState  *pstate = &node->ps;
	ExprContext *econtext = pstate->ps_ExprContext;
	ScanDirection direction = pstate->state->es_direction;
	TupleTableSlot *result_slot;
	TupleTableSlot *slot;
	bool		qual_result;

	Assert(pstate->state->es_epq_active == NULL);
	Assert(projInfo == pstate->ps_ProjInfo);
	Assert(state != NULL);
	pg_assume(state->prefix_qual != NULL);

	ResetExprContext(econtext);
	for (;;)
	{
		slot = ExecScanFetch(node, NULL, accessMtd, recheckMtd);
		if (TupIsNull(slot))
		{
			if (projInfo != NULL)
				return ExecClearTuple(projInfo->pi_state.resultslot);
			return slot;
		}

		econtext->ecxt_scantuple = slot;
		if (!ExecScanBatchReuse(state, slot, direction, &qual_result))
			qual_result = ExecQual(state->prefix_qual, econtext);
		if (qual_result)
			qual_result = ExecQual(state->rest_qual, econtext);

		result_slot = slot;
		if (qual_result && projInfo != NULL)
			result_slot = ExecProject(projInfo);

		if (state->nrows == 0)
			ExecScanBatchPrepare(state, slot, direction, econtext);
		if (qual_result)
			return result_slot;

		InstrCountFiltered1(node, 1);
		ResetExprContext(econtext);
	}
}

#endif							/* EXECSCANBATCH_H */
