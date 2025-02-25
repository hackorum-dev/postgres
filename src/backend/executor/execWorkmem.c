/*-------------------------------------------------------------------------
 *
 * execWorkmem.c
 *	 routine to set the "workmem_limit" field(s) on Plan nodes that need
 *   workimg memory.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execWorkmem.c
 *
 * INTERFACE ROUTINES
 *		ExecAssignWorkMem	- assign working memory to Plan nodes
 *
 *	 NOTES
 *		Historically, every PlanState node, during initialization, looked at
 *		the "work_mem" (plus maybe "hash_mem_multiplier") GUC, to determine
 *		its working-memory limit.
 *
 *		Now, to allow different PlanState nodes to be restricted to different
 *		amounts of memory, each PlanState node reads this limit off the
 *		PlannedStmt's workMemLimits List, at the (1-based) position indicated
 *		by the PlanState's Plan node's "workmem_id" field.
 *
 *		We assign the workmem_id and expand the workMemLimits List, when
 *		creating the Plan node; and then we set this limit by calling
 *		ExecAssignWorkMem(), from InitPlan(), before we initialize the PlanState
 *		nodes.
 *
 * 		The workMemLimit has always applied "per data structure," rather than
 *		"per PlanState". So a single SQL operator (e.g., RecursiveUnion) can
 *		use more than the workMemLimit, even though each of its data
 *		structures is restricted to it.
 *
 *		We store the "workmem_id" field(s) on the Plan, instead of the
 *		PlanState, even though it conceptually belongs to execution rather than
 *		to planning, because we need it to be set before initializing the
 *		corresponding PlanState. This is a chicken-and-egg problem. We could,
 *		of course, make ExecInitNode() a two-phase operation, but that seems
 *		like overkill. Instead, we store these "workmem_id" fields on the Plan,
 *		but set the workMemLimit when we start execution, as part of
 *		InitPlan().
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/plannodes.h"


/* ------------------------------------------------------------------------
 *		ExecAssignWorkMem
 *
 *		Assigns working memory to any Plans or SubPlans that need it.
 *
 *		Inputs:
 *		  'plannedstmt' is the statement to which we assign working memory
 *
 * ------------------------------------------------------------------------
 */
void
ExecAssignWorkMem(PlannedStmt *plannedstmt)
{
	ListCell   *lc_category;
	ListCell   *lc_limit;

	/*
	 * No need to re-assign working memory on parallel workers, since workers
	 * have the same work_mem and hash_mem_multiplier GUCs as the leader.
	 *
	 * We already assigned working-memory limits on the leader, and those
	 * limits were sent to the workers inside the serialized Plan.
	 */
	if (IsParallelWorker())
		return;

	forboth(lc_category, plannedstmt->workMemCategories,
			lc_limit, plannedstmt->workMemLimits)
	{
		lfirst_int(lc_limit) = lfirst_int(lc_category) == WORKMEM_HASH ?
			get_hash_memory_limit() / 1024 : work_mem;
	}
}
