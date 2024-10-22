/*-------------------------------------------------------------------------
 *
 * reorderbuffer_lz4.c
 *	  Functions for ReorderBuffer compression using LZ4.
 *
 * Copyright (c) 2024-2024, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/reorderbuffer_lz4.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_LZ4
#include <lz4.h>
#endif

#include "replication/reorderbuffer_compression.h"

#define NO_LZ4_SUPPORT() \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("compression method lz4 not supported"), \
			 errdetail("This functionality requires the server to be built with lz4 support.")))

/*
 * Allocate a new LZ4StreamingCompressorState.
 */
void *
lz4_NewCompressorState(MemoryContext context)
{
#ifndef USE_LZ4
	NO_LZ4_SUPPORT();
	return NULL;				/* keep compiler quiet */
#else
	LZ4StreamingCompressorState *cstate;
	MemoryContext oldcontext = MemoryContextSwitchTo(context);

	cstate = (LZ4StreamingCompressorState *)
		MemoryContextAlloc(context, sizeof(LZ4StreamingCompressorState));

	cstate->buf = makeStringInfo();

	/*
	 * We do not allocate LZ4 ring buffers and streaming handlers at this
	 * point because we have no guarantee that we will need them later. Let's
	 * allocate only when we are about to use them.
	 */
	cstate->lz4_in_buf = NULL;
	cstate->lz4_out_buf = NULL;
	cstate->lz4_in_buf_offset = 0;
	cstate->lz4_out_buf_offset = 0;
	cstate->lz4_stream = NULL;
	cstate->lz4_stream_decode = NULL;

	MemoryContextSwitchTo(oldcontext);

	return (void *) cstate;
#endif
}

/*
 * Free LZ4 memory resources and the compressor state.
 */
void
lz4_FreeCompressorState(MemoryContext context, void *compressor_state)
{
#ifndef USE_LZ4
	NO_LZ4_SUPPORT();
#else
	LZ4StreamingCompressorState *cstate;
	MemoryContext oldcontext;

	if (compressor_state == NULL)
		return;

	oldcontext = MemoryContextSwitchTo(context);

	cstate = (LZ4StreamingCompressorState *) compressor_state;

	destroyStringInfo(cstate->buf);

	if (cstate->lz4_in_buf != NULL)
	{
		pfree(cstate->lz4_in_buf);
		LZ4_freeStream(cstate->lz4_stream);
	}
	if (cstate->lz4_out_buf != NULL)
	{
		pfree(cstate->lz4_out_buf);
		LZ4_freeStreamDecode(cstate->lz4_stream_decode);
	}

	pfree(compressor_state);

	MemoryContextSwitchTo(oldcontext);
#endif
}

#ifdef USE_LZ4
/*
 * Allocate LZ4 input ring buffer and create the streaming compression handler.
 */
static void
lz4_CreateStreamCompressorState(MemoryContext context, void *compressor_state)
{
	LZ4StreamingCompressorState *cstate;
	MemoryContext oldcontext = MemoryContextSwitchTo(context);

	cstate = (LZ4StreamingCompressorState *) compressor_state;
	cstate->lz4_in_buf = (char *) palloc0(LZ4_RING_BUFFER_SIZE);
	cstate->lz4_stream = LZ4_createStream();

	MemoryContextSwitchTo(oldcontext);
}
#endif

#ifdef USE_LZ4
/*
 * Allocate LZ4 output ring buffer and create the streaming decompression
 */
static void
lz4_CreateStreamDecodeCompressorState(MemoryContext context,
									  void *compressor_state)
{
	LZ4StreamingCompressorState *cstate;
	MemoryContext oldcontext = MemoryContextSwitchTo(context);

	cstate = (LZ4StreamingCompressorState *) compressor_state;
	cstate->lz4_out_buf = (char *) palloc0(LZ4_RING_BUFFER_SIZE);
	cstate->lz4_stream_decode = LZ4_createStreamDecode();

	MemoryContextSwitchTo(oldcontext);
}
#endif

/*
 * Data compression using LZ4 streaming API.
 */
void
lz4_StreamingCompressData(MemoryContext context, char *src, Size src_size,
						  char *dst, Size *dst_size, void *compressor_state)
{
#ifndef USE_LZ4
	NO_LZ4_SUPPORT();
#else
	LZ4StreamingCompressorState *cstate;
	int			lz4_cmp_size = 0;	/* compressed size */
	char	   *lz4_in_bufPtr;	/* input ring buffer pointer */

	cstate = (LZ4StreamingCompressorState *) compressor_state;

	/* Allocate LZ4 input ring buffer and streaming compression handler */
	if (cstate->lz4_in_buf == NULL)
		lz4_CreateStreamCompressorState(context, compressor_state);

	/* Ring buffer offset wraparound */
	if ((cstate->lz4_in_buf_offset + src_size) > LZ4_RING_BUFFER_SIZE)
		cstate->lz4_in_buf_offset = 0;

	/* Get the pointer of the next entry in the ring buffer */
	lz4_in_bufPtr = cstate->lz4_in_buf + cstate->lz4_in_buf_offset;

	/* Copy data that should be compressed into LZ4 input ring buffer */
	memcpy(lz4_in_bufPtr, src, src_size);

	/* Use LZ4 streaming compression API */
	lz4_cmp_size = LZ4_compress_fast_continue(cstate->lz4_stream,
											  lz4_in_bufPtr, dst, src_size,
											  *dst_size, 1);

	if (lz4_cmp_size <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("LZ4 compression failed")));

	/* Move the input ring buffer offset */
	cstate->lz4_in_buf_offset += src_size;

	*dst_size = lz4_cmp_size;
#endif
}

/*
 * Data decompression using LZ4 streaming API.
 * LZ4 decompression uses the output ring buffer to store decompressed data,
 * thus, we don't need to create a new buffer. We return the pointer to data
 * location.
 */
void
lz4_StreamingDecompressData(MemoryContext context, char *src, Size src_size,
							char **dst, Size dst_size, void *compressor_state)
{
#ifndef USE_LZ4
	NO_LZ4_SUPPORT();
#else
	LZ4StreamingCompressorState *cstate;
	char	   *lz4_out_bufPtr; /* output ring buffer pointer */
	int			lz4_dec_size;	/* decompressed data size */

	cstate = (LZ4StreamingCompressorState *) compressor_state;

	/* Allocate LZ4 output ring buffer and streaming decompression handler */
	if (cstate->lz4_out_buf == NULL)
		lz4_CreateStreamDecodeCompressorState(context, compressor_state);

	/* Ring buffer offset wraparound */
	if ((cstate->lz4_out_buf_offset + dst_size) > LZ4_RING_BUFFER_SIZE)
		cstate->lz4_out_buf_offset = 0;

	/* Get current entry pointer in the ring buffer */
	lz4_out_bufPtr = cstate->lz4_out_buf + cstate->lz4_out_buf_offset;

	lz4_dec_size = LZ4_decompress_safe_continue(cstate->lz4_stream_decode,
												src,
												lz4_out_bufPtr,
												src_size,
												dst_size);

	Assert(lz4_dec_size == dst_size);

	if (lz4_dec_size < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed LZ4 data is corrupted")));
	else if (lz4_dec_size != dst_size)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("decompressed LZ4 data size differs from original size")));

	/* Move the output ring buffer offset */
	cstate->lz4_out_buf_offset += lz4_dec_size;

	/* Point to the decompressed data location */
	*dst = lz4_out_bufPtr;
#endif
}

Size
lz4_CompressBound(Size src_size)
{
#ifndef USE_LZ4
	NO_LZ4_SUPPORT();
	return -1;
#else
	return LZ4_COMPRESSBOUND(src_size);
#endif
}

/*
 * Returns the StringInfo buffer we use to store compressed/decompressed data.
 */
StringInfo
lz4_GetStringInfoBuffer(void *compressor_state)
{
#ifndef USE_LZ4
	NO_LZ4_SUPPORT();
	return NULL;
#else
	LZ4StreamingCompressorState *cstate;

	cstate = (LZ4StreamingCompressorState *) compressor_state;

	return cstate->buf;
#endif
}
