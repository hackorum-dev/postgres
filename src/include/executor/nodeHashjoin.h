/*-------------------------------------------------------------------------
 *
 * nodeHashjoin.h
 *	  prototypes for nodeHashjoin.c
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeHashjoin.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEHASHJOIN_H
#define NODEHASHJOIN_H

#include "access/parallel.h"
#include "nodes/execnodes.h"
#include "storage/buffile.h"

extern HashJoinState *ExecInitHashJoin(HashJoin *node, EState *estate, int eflags);
extern void ExecEndHashJoin(HashJoinState *node);
extern void ExecReScanHashJoin(HashJoinState *node);
extern void ExecShutdownHashJoin(HashJoinState *node);
extern void ExecHashJoinEstimate(HashJoinState *state, ParallelContext *pcxt);
extern void ExecHashJoinInitializeDSM(HashJoinState *state, ParallelContext *pcxt);
extern void ExecHashJoinReInitializeDSM(HashJoinState *state, ParallelContext *pcxt);
extern void ExecHashJoinInitializeWorker(HashJoinState *state,
										 ParallelWorkerContext *pwcxt);
extern void ExecHashJoinRetrieveInstrumentation(HashJoinState *node);
extern void ExecHashJoinGetInstrumentation(HashInstrumentation *instrument,
										   HashJoinTable hashtable);
extern void ExecChooseHashTableSize(double ntuples, int tupwidth, bool useskew,
									bool try_combined_work_mem,
									int parallel_workers,
									size_t *space_allowed,
									int *numbuckets,
									int *numbatches,
									int *num_skew_mcvs);

#endif							/* NODEHASHJOIN_H */
