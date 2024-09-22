/*-------------------------------------------------------------------------
 *
 * reorderbuffer_pglz.c
 *	  Functions used for ReorderBuffer compression using PGLZ.
 *
 * Copyright (c) 2024-2024, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/reorderbuffer_pglz.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "replication/reorderbuffer_compression.h"

/*
 * Allocate a new PGLZCompressorState.
 */
void *
pglz_NewCompressorState(MemoryContext context)
{
	PGLZCompressorState *cstate;
	MemoryContext oldcontext = MemoryContextSwitchTo(context);

	cstate = (PGLZCompressorState *)
		MemoryContextAlloc(context, sizeof(PGLZCompressorState));

	cstate->buf = makeStringInfo();

	MemoryContextSwitchTo(oldcontext);

	return (void *) cstate;
}

/*
 * Free PGLZ memory resources and compressor state.
 */
void
pglz_FreeCompressorState(MemoryContext context, void *compressor_state)
{
	PGLZCompressorState *cstate;
	MemoryContext oldcontext;

	if (compressor_state == NULL)
		return;

	oldcontext = MemoryContextSwitchTo(context);

	cstate = (PGLZCompressorState *) compressor_state;

	destroyStringInfo(cstate->buf);
	pfree(compressor_state);

	MemoryContextSwitchTo(oldcontext);
}
