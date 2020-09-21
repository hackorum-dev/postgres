/*-------------------------------------------------------------------------
 *
 * nodeBitmapIndexscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeBitmapIndexscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEBITMAPINDEXSCAN_H
#define NODEBITMAPINDEXSCAN_H

#include "access/parallel.h"
#include "nodes/execnodes.h"

extern BitmapIndexScanState *ExecInitBitmapIndexScan(BitmapIndexScan *node, EState *estate, int eflags);
extern Node *MultiExecBitmapIndexScan(BitmapIndexScanState *node);
extern void ExecEndBitmapIndexScan(BitmapIndexScanState *node);
extern void ExecReScanBitmapIndexScan(BitmapIndexScanState *node);
extern void ExecBitmapIndexEstimate(BitmapIndexScanState *node,
									ParallelContext *pcxt);
extern void ExecBitmapIndexInitializeDSM(BitmapIndexScanState *node,
										 ParallelContext *pcxt);
extern void ExecBitmapIndexInitializeWorker(BitmapIndexScanState *node,
											ParallelWorkerContext *pwcxt);
extern void ExecBitmapIndexReInitializeDSM(BitmapIndexScanState *node,
										   ParallelContext *pcxt);

#endif							/* NODEBITMAPINDEXSCAN_H */
