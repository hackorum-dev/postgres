/*-------------------------------------------------------------------------
 *
 * execScan.c
 *	  This code provides support for generalized relation scans. ExecScan
 *	  is passed a node and a pointer to a function to "do the right thing"
 *	  and return a tuple from the relation. ExecScan then does the tedious
 *	  stuff - checking the qualification and projecting the tuple
 *	  appropriately.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execScan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "execScanBatch.h"
#include "executor/executor.h"
#include "executor/execScan.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "pgstat.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

static Node *
strip_batch_qual_relabel(Node *node)
{
	if (IsA(node, RelabelType))
		node = (Node *) castNode(RelabelType, node)->arg;
	return node;
}

static bool
is_batch_qual_value(Node *node)
{
	return IsA(node, Const) ||
		(IsA(node, Param) &&
		 (castNode(Param, node)->paramkind == PARAM_EXTERN ||
		  castNode(Param, node)->paramkind == PARAM_EXEC));
}

static bool
is_batch_qual_function(Oid funcid)
{
	HeapTuple	proctup;
	Form_pg_proc procform;
	bool		result;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(proctup);
	result = procform->prorettype == BOOLOID && !procform->proretset &&
		procform->provolatile == PROVOLATILE_IMMUTABLE &&
		procform->proisstrict && procform->proleakproof;
	ReleaseSysCache(proctup);

	return result;
}

static OpExpr *
match_batch_qual(Node *clause, uint8 *var_argno)
{
	OpExpr	   *op;
	Node	   *args[2];
	Var		   *var;

	if (!IsA(clause, OpExpr))
		return NULL;
	op = (OpExpr *) clause;
	if (list_length(op->args) != 2)
		return NULL;

	args[0] = strip_batch_qual_relabel(linitial(op->args));
	args[1] = strip_batch_qual_relabel(lsecond(op->args));
	if (IsA(args[0], Var) && is_batch_qual_value(args[1]))
		*var_argno = 0;
	else if (is_batch_qual_value(args[0]) && IsA(args[1], Var))
		*var_argno = 1;
	else
		return NULL;
	var = castNode(Var, args[*var_argno]);

	if (var->varno == INNER_VAR || var->varno == OUTER_VAR ||
		var->varattno <= 0 || var->varlevelsup != 0 ||
		var->varreturningtype != VAR_RETURNING_DEFAULT)
		return NULL;

	return op;
}

static void
init_batch_qual(struct ExecScanBatchQual *qual, OpExpr *op,
				uint8 var_argno, PlanState *parent)
{
	Node	   *arg;
	Var		   *var;

	arg = strip_batch_qual_relabel(list_nth(op->args, 1 - var_argno));
	var = castNode(Var,
				   strip_batch_qual_relabel(list_nth(op->args, var_argno)));
	fmgr_info(op->opfuncid, &qual->func);
	fmgr_info_set_expr((Node *) op, &qual->func);
	qual->fcinfo = palloc0(SizeForFunctionCallInfo(2));
	InitFunctionCallInfoData(*qual->fcinfo, &qual->func, 2,
							 op->inputcollid, NULL, NULL);
	if (IsA(arg, Const))
	{
		qual->constant.value = castNode(Const, arg)->constvalue;
		qual->constant.isnull = castNode(Const, arg)->constisnull;
	}
	else
	{
		Assert(IsA(arg, Param));
		qual->param_expr = ExecInitExpr((Expr *) arg, parent);
	}
	qual->attnum = var->varattno;
	qual->var_argno = var_argno;
}

static Datum
ExecScanBatchQual(ExprState *state, ExprContext *econtext, bool *isnull)
{
	ExecScanBatchState *batch_state = state->evalfunc_private;
	bool		result;

	result = ExecQual(batch_state->prefix_qual, econtext);
	if (result)
		result = ExecQual(batch_state->rest_qual, econtext);
	*isnull = false;

	return BoolGetDatum(result);
}

/*
 * Initialize batch evaluation for the supported initial clauses. The scan
 * expression context and slot must already be initialized. On success,
 * node->ps.qual is initialized to evaluate the complete clause list.
 */
ExecScanBatchState *
ExecInitScanBatch(ScanState *node, List *clauses)
{
	ExecScanBatchState *state;
	ExprState  *qual;
	Bitmapset  *attrs = NULL;
	int			nquals = 0;
	int			i = 0;

	Assert(node->ps.qual == NULL);
	Assert(node->ps.ps_ExprContext != NULL);
	if (!slot_supports_batch(node->ss_ScanTupleSlot))
		return NULL;

	/*
	 * Count the supported prefix before allocating an exact-sized state
	 * array.
	 */
	foreach_ptr(Node, clause, clauses)
	{
		OpExpr	   *op;
		uint8		var_argno;

		op = match_batch_qual(clause, &var_argno);
		if (op == NULL || !is_batch_qual_function(op->opfuncid))
			break;
		nquals++;
	}
	if (nquals == 0)
		return NULL;

	state = palloc0_object(ExecScanBatchState);
	state->quals = palloc0_array(struct ExecScanBatchQual, nquals);

	/*
	 * Do not make this a child of ecxt_per_tuple_memory: resetting the
	 * expression context would delete the child and leave this pointer
	 * dangling.
	 */
	state->operator_context =
		AllocSetContextCreate(node->ps.ps_ExprContext->ecxt_per_query_memory,
							  "ExecScanBatch operator",
							  ALLOCSET_START_SMALL_SIZES);
	state->prefix_qual =
		ExecInitQual(list_copy_head(clauses, nquals), &node->ps);
	state->rest_qual =
		ExecInitQual(list_copy_tail(clauses, nquals), &node->ps);
	state->nquals = nquals;
	foreach_ptr(Node, clause, clauses)
	{
		OpExpr	   *op;
		uint8		var_argno;

		if (i >= nquals)
			break;
		op = match_batch_qual(clause, &var_argno);
		Assert(op != NULL);
		init_batch_qual(&state->quals[i], op, var_argno, &node->ps);
		attrs = bms_add_member(attrs, state->quals[i].attnum - 1);
		i++;
	}
	state->natts = bms_num_members(attrs);
	state->attnums = palloc_array(AttrNumber, state->natts);
	{
		int			attnum = -1;

		i = 0;
		while ((attnum = bms_next_member(attrs, attnum)) >= 0)
			state->attnums[i++] = attnum + 1;
		Assert(i == state->natts);
	}
	for (i = 0; i < nquals; i++)
	{
		state->quals[i].attr_index =
			bms_member_index(attrs, state->quals[i].attnum - 1);
		Assert(state->quals[i].attr_index >= 0);
	}
	bms_free(attrs);

	state->values =
		palloc_array(Datum,
					 (Size) state->natts * TUPLE_BATCH_MASK_BITS);
	state->isnull =
		palloc_array(bool,
					 (Size) state->natts * TUPLE_BATCH_MASK_BITS);

	/* Use the same scalar states when the complete qual must be evaluated. */
	qual = makeNode(ExprState);
	qual->flags = EEO_FLAG_IS_QUAL;
	qual->expr = (Expr *) clauses;
	qual->evalfunc = ExecScanBatchQual;
	qual->evalfunc_private = state;
	qual->parent = &node->ps;
	node->ps.qual = qual;

	return state;
}

void
ExecResetScanBatch(ExecScanBatchState *state)
{
	if (state == NULL)
		return;

	state->batch = NULL;
	state->nrows = 0;
}

static pg_always_inline bool
eval_batch_qual(struct ExecScanBatchQual *qual, Datum value,
				bool track_function)
{
	FunctionCallInfo fcinfo = qual->fcinfo;
	PgStat_FunctionCallUsage fcusage;
	Datum		result;

	fcinfo->args[qual->var_argno].value = value;
	fcinfo->args[qual->var_argno].isnull = false;
	Assert(!fcinfo->args[1 - qual->var_argno].isnull);
	if (track_function)
		pgstat_init_function_usage(fcinfo, &fcusage);
	fcinfo->isnull = false;
	result = FunctionCallInvoke(fcinfo);
	if (track_function)
		pgstat_end_function_usage(&fcusage, true);

	return !fcinfo->isnull && DatumGetBool(result);
}

void
ExecScanBatchPrepare(ExecScanBatchState *state, TupleTableSlot *slot,
					 ScanDirection direction, ExprContext *econtext)
{
	const TupleTableSlotBatch *batch = slot_getbatch(slot);
	MemoryContext oldcontext;
	TupleBatchMask mask;
	int			first;
	int			nrows;

	Assert(ScanDirectionIsValid(direction));
	ExecResetScanBatch(state);
	if (batch == NULL || ScanDirectionIsNoMovement(direction))
		return;
	Assert(state->nquals > 0);

	if (ScanDirectionIsForward(direction))
	{
		first = batch->current + 1;
		nrows = Min((int) TUPLE_BATCH_MASK_BITS,
					batch->ntuples - first);
	}
	else
	{
		nrows = Min((int) TUPLE_BATCH_MASK_BITS, batch->current);
		first = batch->current - nrows;
	}
	if (nrows == 0)
		return;
	mask = PG_UINT64_MAX >> (TUPLE_BATCH_MASK_BITS - nrows);
	batch->getsomeattrs(slot, state->attnums, state->natts, first, nrows,
						state->values, state->isnull);

	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	for (int q = 0; q < state->nquals && mask != 0; q++)
	{
		struct ExecScanBatchQual *qual = &state->quals[q];
		FunctionCallInfo fcinfo = qual->fcinfo;
		NullableDatum *other_arg = &fcinfo->args[1 - qual->var_argno];

		if (qual->param_expr != NULL)
			other_arg->value = ExecEvalExpr(qual->param_expr, econtext,
											&other_arg->isnull);
		else
			*other_arg = qual->constant;

		if (other_arg->isnull)
			mask = 0;
		else
		{
			MemoryContext oldoperatorcontext;
			bool		track_function;
			int			position;

			track_function =
				pgstat_track_functions > fcinfo->flinfo->fn_stats;
			position = ScanDirectionIsForward(direction) ? 0 : nrows - 1;
			oldoperatorcontext =
				MemoryContextSwitchTo(state->operator_context);
			for (int i = 0; i < nrows && mask != 0;
				 i++, position += direction)
			{
				TupleBatchMask bit = UINT64CONST(1) << position;
				int			value_index =
					qual->attr_index * TUPLE_BATCH_MASK_BITS + position;

				if ((mask & bit) == 0)
					continue;
				if (state->isnull[value_index] ||
					!eval_batch_qual(qual, state->values[value_index],
									 track_function))
					mask &= ~bit;
			}
			MemoryContextSwitchTo(oldoperatorcontext);
		}

		if (qual->param_expr != NULL)
		{
			other_arg->value = (Datum) 0;
			other_arg->isnull = true;
		}

		/*
		 * Functions run in operator_context and may leave temporary
		 * allocations there. Reset once after the dense loop to amortize the
		 * reset overhead, while limiting retained memory to one batch for a
		 * single qual.
		 */
		MemoryContextReset(state->operator_context);
	}
	MemoryContextSwitchTo(oldcontext);

	state->batch = batch;
	state->generation = batch->generation;
	state->mask = mask;
	state->first = first;
	state->nrows = nrows;
}

/* ----------------------------------------------------------------
 *		ExecScan
 *
 *		Scans the relation using the 'access method' indicated and
 *		returns the next qualifying tuple.
 *		The access method returns the next tuple and ExecScan() is
 *		responsible for checking the tuple returned against the qual-clause.
 *
 *		A 'recheck method' must also be provided that can check an
 *		arbitrary tuple of the relation against any qual conditions
 *		that are implemented internal to the access method.
 *
 *		Conditions:
 *		  -- the "cursor" maintained by the AMI is positioned at the tuple
 *			 returned previously.
 *
 *		Initial States:
 *		  -- the relation indicated is opened for scanning so that the
 *			 "cursor" is positioned before the first qualifying tuple.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecScan(ScanState *node,
		 ExecScanAccessMtd accessMtd,	/* function returning a tuple */
		 ExecScanRecheckMtd recheckMtd)
{
	EPQState   *epqstate;
	ExprState  *qual;
	ProjectionInfo *projInfo;

	epqstate = node->ps.state->es_epq_active;
	qual = node->ps.qual;
	projInfo = node->ps.ps_ProjInfo;

	return ExecScanExtended(node,
							accessMtd,
							recheckMtd,
							epqstate,
							qual,
							projInfo);
}

/*
 * ExecScanBatch
 *		ExecScan variant that evaluates a supported initial run of scan
 *		qualifications using tuple batches.
 *
 * A NULL state means that batch evaluation was unavailable, so use the
 * ordinary path. Batch state cannot be used for EvalPlanQual tuples either.
 * Callers must use ExecResetScanBatch() when rescanning the node.
 *
 * This entry point is intended for scan nodes implemented by extensions.
 * Built-in nodes may call ExecScanBatchExtended() directly so the compiler
 * can inline their access method and specialize projection handling.
 */
TupleTableSlot *
ExecScanBatch(ScanState *node,
			  ExecScanAccessMtd accessMtd,
			  ExecScanRecheckMtd recheckMtd,
			  ExecScanBatchState *state)
{
	if (state == NULL)
		return ExecScan(node, accessMtd, recheckMtd);

	if (node->ps.state->es_epq_active != NULL)
	{
		ExecResetScanBatch(state);
		return ExecScan(node, accessMtd, recheckMtd);
	}

	return ExecScanBatchExtended(node, accessMtd, recheckMtd, state,
								 node->ps.ps_ProjInfo);
}

/*
 * ExecAssignScanProjectionInfo
 *		Set up projection info for a scan node, if necessary.
 *
 * We can avoid a projection step if the requested tlist exactly matches
 * the underlying tuple type.  If so, we just set ps_ProjInfo to NULL.
 * Note that this case occurs not only for simple "SELECT * FROM ...", but
 * also in most cases where there are joins or other processing nodes above
 * the scan node, because the planner will preferentially generate a matching
 * tlist.
 *
 * The scan slot's descriptor must have been set already.
 */
void
ExecAssignScanProjectionInfo(ScanState *node)
{
	Scan	   *scan = (Scan *) node->ps.plan;
	TupleDesc	tupdesc = node->ss_ScanTupleSlot->tts_tupleDescriptor;

	ExecConditionalAssignProjectionInfo(&node->ps, tupdesc, scan->scanrelid);
}

/*
 * ExecAssignScanProjectionInfoWithVarno
 *		As above, but caller can specify varno expected in Vars in the tlist.
 */
void
ExecAssignScanProjectionInfoWithVarno(ScanState *node, int varno)
{
	TupleDesc	tupdesc = node->ss_ScanTupleSlot->tts_tupleDescriptor;

	ExecConditionalAssignProjectionInfo(&node->ps, tupdesc, varno);
}

/*
 * ExecScanReScan
 *
 * This must be called within the ReScan function of any plan node type
 * that uses ExecScan().
 */
void
ExecScanReScan(ScanState *node)
{
	EState	   *estate = node->ps.state;

	/*
	 * We must clear the scan tuple so that observers (e.g., execCurrent.c)
	 * can tell that this plan node is not positioned on a tuple.
	 */
	ExecClearTuple(node->ss_ScanTupleSlot);

	/*
	 * Rescan EvalPlanQual tuple(s) if we're inside an EvalPlanQual recheck.
	 * But don't lose the "blocked" status of blocked target relations.
	 */
	if (estate->es_epq_active != NULL)
	{
		EPQState   *epqstate = estate->es_epq_active;
		Index		scanrelid = ((Scan *) node->ps.plan)->scanrelid;

		if (scanrelid > 0)
			epqstate->relsubs_done[scanrelid - 1] =
				epqstate->relsubs_blocked[scanrelid - 1];
		else
		{
			Bitmapset  *relids;
			int			rtindex = -1;

			/*
			 * If an FDW or custom scan provider has replaced the join with a
			 * scan, there are multiple RTIs; reset the relsubs_done flag for
			 * all of them.
			 */
			if (IsA(node->ps.plan, ForeignScan))
				relids = ((ForeignScan *) node->ps.plan)->fs_base_relids;
			else if (IsA(node->ps.plan, CustomScan))
				relids = ((CustomScan *) node->ps.plan)->custom_relids;
			else
				elog(ERROR, "unexpected scan node: %d",
					 (int) nodeTag(node->ps.plan));

			while ((rtindex = bms_next_member(relids, rtindex)) >= 0)
			{
				Assert(rtindex > 0);
				epqstate->relsubs_done[rtindex - 1] =
					epqstate->relsubs_blocked[rtindex - 1];
			}
		}
	}
}
