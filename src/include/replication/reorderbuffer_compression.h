/*-------------------------------------------------------------------------
 *
 * reorderbuffer_compression.h
 *	  ReorderBuffer spill files compression.
 *
 * Copyright (c) 2024-2024, PostgreSQL Global Development Group
 *
 * src/include/access/reorderbuffer_compression.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef REORDERBUFFER_COMPRESSION_H
#define REORDERBUFFER_COMPRESSION_H

/* ReorderBuffer on disk compression algorithms */
typedef enum ReorderBufferCompressionMethod
{
	REORDER_BUFFER_NO_COMPRESSION,
	REORDER_BUFFER_PGLZ_COMPRESSION,
}			ReorderBufferCompressionMethod;

/*
 * Compression strategy applied to ReorderBuffer records spilled on disk
 */
typedef enum ReorderBufferCompressionStrategy
{
	REORDER_BUFFER_STRAT_UNCOMPRESSED,
	REORDER_BUFFER_STRAT_PGLZ,
}			ReorderBufferCompressionStrategy;

typedef struct PGLZCompressorState
{
	/* Buffer used to store compressed data */
	StringInfo	buf;
}			PGLZCompressorState;

extern void *pglz_NewCompressorState(MemoryContext context);
extern void pglz_FreeCompressorState(MemoryContext context,
									 void *compressor_state);

#endif							/* REORDERBUFFER_COMPRESSION_H */
