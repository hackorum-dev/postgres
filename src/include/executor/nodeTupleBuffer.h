/*-------------------------------------------------------------------------
 *
 * nodeTupleBuffer.h
 *	  Executor support for eager tuple buffering.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/include/executor/nodeTupleBuffer.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODETUPLEBUFFER_H
#define NODETUPLEBUFFER_H

#include "nodes/execnodes.h"

extern TupleBufferState *ExecInitTupleBuffer(TupleBuffer *node, EState *estate,
											 int eflags);
extern void ExecEndTupleBuffer(TupleBufferState *node);
extern void ExecReScanTupleBuffer(TupleBufferState *node);
extern void ExecTupleBufferMarkPos(TupleBufferState *node);
extern void ExecTupleBufferRestrPos(TupleBufferState *node);

#endif							/* NODETUPLEBUFFER_H */
