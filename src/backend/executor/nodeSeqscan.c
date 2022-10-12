/*-------------------------------------------------------------------------
 *
 * nodeSeqscan.c
 *	  Support routines for sequential scans of relations.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSeqscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecSeqScan				sequentially scans a relation.
 *		ExecSeqNext				retrieve next tuple in sequential order.
 *		ExecInitSeqScan			creates and initializes a seqscan node.
 *		ExecEndSeqScan			releases any storage allocated.
 *		ExecReScanSeqScan		rescans the relation
 *
 *		ExecSeqScanEstimate		estimates DSM space needed for parallel scan
 *		ExecSeqScanInitializeDSM initialize DSM for parallel scan
 *		ExecSeqScanReInitializeDSM reinitialize DSM for fresh parallel scan
 *		ExecSeqScanInitializeWorker attach to DSM info in parallel worker
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/tableam.h"
#include "executor/execdebug.h"
#include "executor/nodeMergejoin.h"
#include "executor/nodeSeqscan.h"
#include "utils/rel.h"
#include "storage/lwlock.h"
#include "storage/shm_toc.h"
#include <unistd.h>

/* Magic number for location of shared dsa pointer if scan is using a semi-join filter */
#define DSA_LOCATION_KEY_FOR_SJF	UINT64CONST(0xE00000000000FFFF)

static TupleTableSlot *SeqNext(SeqScanState *node);

/* ----------------------------------------------------------------
 *						Scan Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		SeqNext
 *
 *		This is a workhorse for ExecSeqScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
SeqNext(SeqScanState *node)
{
	TableScanDesc scandesc;
	EState	   *estate;
	ScanDirection direction;
	TupleTableSlot *slot;

	/*
	 * get information from the estate and scan state
	 */
	scandesc = node->ss.ss_currentScanDesc;
	estate = node->ss.ps.state;
	direction = estate->es_direction;
	slot = node->ss.ss_ScanTupleSlot;

	if (scandesc == NULL)
	{
		/*
		 * We reach here if the scan is not parallel, or if we're serially
		 * executing a scan that was planned to be parallel.
		 */
		scandesc = table_beginscan(node->ss.ss_currentRelation,
								   estate->es_snapshot,
								   0, NULL);
		node->ss.ss_currentScanDesc = scandesc;
	}

	/*
	 * get the next tuple from the table
	 */
	if (table_scan_getnextslot(scandesc, direction, slot))
		return slot;
	return NULL;
}

/*
 * SeqRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
SeqRecheck(SeqScanState *node, TupleTableSlot *slot)
{
	/*
	 * Note that unlike IndexScan, SeqScan never use keys in heap_beginscan
	 * (and this is very bad) - so, here we do not check are keys ok or not.
	 */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecSeqScan(node)
 *
 *		Scans the relation sequentially and returns the next qualifying
 *		tuple.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecSeqScan(PlanState *pstate)
{
	SeqScanState *node = castNode(SeqScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) SeqNext,
					(ExecScanRecheckMtd) SeqRecheck);
}


/* ----------------------------------------------------------------
 *		ExecInitSeqScan
 * ----------------------------------------------------------------
 */
SeqScanState *
ExecInitSeqScan(SeqScan *node, EState *estate, int eflags)
{
	SeqScanState *scanstate;

	/*
	 * Once upon a time it was possible to have an outerPlan of a SeqScan, but
	 * not any more.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	scanstate = makeNode(SeqScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecSeqScan;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * open the scan relation
	 */
	scanstate->ss.ss_currentRelation =
		ExecOpenScanRelation(estate,
							 node->scan.scanrelid,
							 eflags);

	/* and create slot with the appropriate rowtype */
	ExecInitScanTupleSlot(estate, &scanstate->ss,
						  RelationGetDescr(scanstate->ss.ss_currentRelation),
						  table_slot_callbacks(scanstate->ss.ss_currentRelation));

	/*
	 * Initialize result type and projection.
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);

	/*
	 * Initialize semijoin filter expressions.
	 */
	if (node->scan.plan.sj_md_list)
	{
		ListCell   *lc;

		scanstate->sj_scan_data = NIL;
		foreach(lc, node->scan.plan.sj_md_list)
		{
			SemijoinFilterScanData *md = (SemijoinFilterScanData *) lfirst(lc);
			SemiJoinFilterScanNodeState *sj_filter = (SemiJoinFilterScanNodeState *) palloc0(sizeof(SemiJoinFilterScanNodeState));

			sj_filter->is_building_side = md->is_building_node;
			sj_filter->expr_state = ExecInitExpr(lfirst(list_nth_cell(md->semijoin_keys, 0)), (PlanState *) scanstate);
			scanstate->sj_scan_data = lappend(scanstate->sj_scan_data, sj_filter);
		}
	}

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndSeqScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndSeqScan(SeqScanState *node)
{
	TableScanDesc scanDesc;

	/*
	 * get information from node
	 */
	scanDesc = node->ss.ss_currentScanDesc;

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * clean out the tuple table
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * close heap scan
	 */
	if (scanDesc != NULL)
		table_endscan(scanDesc);
}

/* ----------------------------------------------------------------
 *						Join Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecReScanSeqScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanSeqScan(SeqScanState *node)
{
	TableScanDesc scan;

	scan = node->ss.ss_currentScanDesc;

	if (scan != NULL)
		table_rescan(scan,		/* scan desc */
					 NULL);		/* new scan keys */

	ExecScanReScan((ScanState *) node);
}

/* ----------------------------------------------------------------
 *						Parallel Scan Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecSeqScanEstimate
 *
 *		Compute the amount of space we'll need in the parallel
 *		query DSM, and inform pcxt->estimator about our needs.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanEstimate(SeqScanState *node,
					ParallelContext *pcxt)
{
	EState	   *estate = node->ss.ps.state;

	node->pscan_len = table_parallelscan_estimate(node->ss.ss_currentRelation,
												  estate->es_snapshot);
	shm_toc_estimate_chunk(&pcxt->estimator, node->pscan_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/*
	 * Estimate space for extra dsa_pointer address for when parallel
	 * sequential scans use a semi-join filter.
	 */
	if (node->ss.ps.plan->parallel_aware && node->apply_semijoin_filter)
	{
		shm_toc_estimate_keys(&pcxt->estimator, 1);
		if (node->semijoin_filters)
		{
			shm_toc_estimate_keys(&pcxt->estimator, sizeof(dsa_pointer) * list_length(node->semijoin_filters));
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecSeqScanInitializeDSM
 *
 *		Set up a parallel heap scan descriptor.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanInitializeDSM(SeqScanState *node,
						 ParallelContext *pcxt)
{
	EState	   *estate = node->ss.ps.state;
	ParallelTableScanDesc pscan;

	/*
	 * If scan is using a semi-join filter, then initialize dsa pointer of
	 * shared sjf
	 */
	if (node->apply_semijoin_filter)
	{
		int			sjf_num = list_length(node->semijoin_filters);
		dsa_pointer *dsa_pointer_address;	/* actuallly an array of size
											 * sjf_num */
		ListCell   *lc;
		int			i = 0;

		dsa_pointer_address = (dsa_pointer *) shm_toc_allocate(pcxt->toc, sizeof(dsa_pointer) * sjf_num);
		foreach(lc, node->semijoin_filters)
		{
			SemiJoinFilterJoinNodeState *sjf = (SemiJoinFilterJoinNodeState *) (lfirst(lc));
			SemiJoinFilterParallelState *parallel_state;
			dsa_area   *area = node->ss.ps.state->es_query_dsa;

			sjf->parallel_state = CreateFilterParallelState(area, sjf, sjf_num);
			sjf->is_parallel = true;
			/* check if main process always will run */
			parallel_state = (SemiJoinFilterParallelState *) dsa_get_address(area, sjf->parallel_state);
			parallel_state->num_processes = 1;
			/* update parallel_state with built bloom filter */
			if (sjf->done_building && node->ss.ps.plan->plan_node_id == sjf->checking_node_id)
			{
				bloom_filter *parallel_bloom = (bloom_filter *) dsa_get_address(area, parallel_state->bloom_dsa_address);

				replace_bitset(parallel_bloom, sjf->filter);
				LWLockRelease(&parallel_state->secondlock);
			}
			dsa_pointer_address[i] = sjf->parallel_state;
			i++;
		}

		/*
		 * add plan_id to magic number so this is also unique for each plan
		 * node
		 */
		shm_toc_insert(pcxt->toc, DSA_LOCATION_KEY_FOR_SJF +
					   node->ss.ps.plan->plan_node_id, dsa_pointer_address);
	}

	pscan = shm_toc_allocate(pcxt->toc, node->pscan_len);
	table_parallelscan_initialize(node->ss.ss_currentRelation,
								  pscan,
								  estate->es_snapshot);
	shm_toc_insert(pcxt->toc, node->ss.ps.plan->plan_node_id, pscan);
	node->ss.ss_currentScanDesc =
		table_beginscan_parallel(node->ss.ss_currentRelation, pscan);
}

/* ----------------------------------------------------------------
 *		ExecSeqScanReInitializeDSM
 *
 *		Reset shared state before beginning a fresh scan.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanReInitializeDSM(SeqScanState *node,
						   ParallelContext *pcxt)
{
	ParallelTableScanDesc pscan;

	pscan = node->ss.ss_currentScanDesc->rs_parallel;
	table_parallelscan_reinitialize(node->ss.ss_currentRelation, pscan);
}

/* ----------------------------------------------------------------
 *		ExecSeqScanInitializeWorker
 *
 *		Copy relevant information from TOC into planstate.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanInitializeWorker(SeqScanState *node,
							ParallelWorkerContext *pwcxt)
{
	ParallelTableScanDesc pscan;

	/*
	 * Create worker's semi-join filter for merge join, if using it. We first
	 * need to check shm_toc to see if a sjf exists, then create the local
	 * backend sjf.
	 */
	if (shm_toc_lookup(pwcxt->toc, DSA_LOCATION_KEY_FOR_SJF + node->ss.ps.plan->plan_node_id, 1))
	{
		dsa_pointer *parallel_addresses = (dsa_pointer *)
		shm_toc_lookup(pwcxt->toc, DSA_LOCATION_KEY_FOR_SJF + node->ss.ps.plan->plan_node_id, 1);

		/*
		 * we know that there is at least one sjf, we will update accordingly
		 * if the parallel state says there is more (this avoids using an
		 * additional shm_toc allocation)
		 */
		int			sjf_num = 1;

		/*
		 * If a copy of any sjf already exists on the backend, we want to free
		 * it and create a new one.
		 */
		if (node->apply_semijoin_filter)
		{
			while (list_length(node->semijoin_filters) > 0)
			{
				SemiJoinFilterJoinNodeState *sjf = (SemiJoinFilterJoinNodeState *) (list_head(node->semijoin_filters)->ptr_value);

				node->semijoin_filters = list_delete_nth_cell(node->semijoin_filters, 0);
				FreeSemiJoinFilter(sjf);
			}
		}

		/*
		 * Here, we create the process-local SJF's, which will later be
		 * combined into the single SJF after all parallel work is done.
		 */
		for (int i = 0; i < sjf_num; i++)
		{
			dsa_pointer parallel_address = parallel_addresses[i];
			SemiJoinFilterParallelState *parallel_state = (SemiJoinFilterParallelState *)
			dsa_get_address(node->ss.ps.state->es_query_dsa, parallel_address);
			SemiJoinFilterJoinNodeState *sjf;
			MemoryContext oldContext;

			sjf_num = parallel_state->sjf_num;
			oldContext = MemoryContextSwitchTo(GetMemoryChunkContext(node));
			sjf = (SemiJoinFilterJoinNodeState *) palloc0(sizeof(SemiJoinFilterJoinNodeState));
			sjf->filter = bloom_create(parallel_state->num_elements, parallel_state->work_mem,
									   parallel_state->seed);
			sjf->building_node_id = parallel_state->building_node_id;
			sjf->checking_node_id = parallel_state->checking_node_id;
			sjf->seed = parallel_state->seed;
			sjf->is_parallel = true;
			sjf->is_worker = true;
			sjf->done_building = parallel_state->done_building;
			sjf->parallel_state = parallel_address;
			node->apply_semijoin_filter = true;
			node->semijoin_filters = lappend(node->semijoin_filters, (void *) sjf);
			sjf->mergejoin_plan_id = parallel_state->mergejoin_plan_id;
			/* copy over bloom filter if already built */
			if (sjf->done_building && parallel_state->checking_node_id == node->ss.ps.plan->plan_node_id)
			{
				SemiJoinFilterParallelState *copy_parallel_state = (SemiJoinFilterParallelState *)
				dsa_get_address(node->ss.ps.state->es_query_dsa, sjf->parallel_state);
				bloom_filter *shared_bloom = (bloom_filter *) dsa_get_address(
																			  node->ss.ps.state->es_query_dsa, copy_parallel_state->bloom_dsa_address);

				replace_bitset(sjf->filter, shared_bloom);
			}
			else if (!sjf->done_building && parallel_state->building_node_id == node->ss.ps.plan->plan_node_id)
			{
				/*
				 * Add this process to number of scan processes, need to use
				 * lock in case of multiple workers updating at same time. We
				 * want to avoid using the planned number of workers because
				 * that can be wrong.
				 */
				LWLockAcquire(&parallel_state->lock, LW_EXCLUSIVE);
				parallel_state->num_processes += 1;
				LWLockRelease(&parallel_state->lock);
			}
			MemoryContextSwitchTo(oldContext);
		}
	}

	pscan = shm_toc_lookup(pwcxt->toc, node->ss.ps.plan->plan_node_id, false);
	node->ss.ss_currentScanDesc =
		table_beginscan_parallel(node->ss.ss_currentRelation, pscan);
}
