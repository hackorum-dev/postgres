/*-------------------------------------------------------------------------
 *
 * reorderbuffer_zstd.c
 *	  Functions for ReorderBuffer compression using ZSTD.
 *
 * Copyright (c) 2024-2024, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/reorderbuffer_zstd.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_ZSTD
#include <zstd.h>
#endif

#include "replication/reorderbuffer_compression.h"

#define NO_ZSTD_SUPPORT() \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("compression method zstd not supported"), \
			 errdetail("This functionality requires the server to be built with zstd support.")))

/*
 * Allocate a new ZSTDStreamingCompressorState.
 */
void *
zstd_NewCompressorState(MemoryContext context)
{
#ifndef USE_ZSTD
	NO_ZSTD_SUPPORT();
	return NULL;				/* keep compiler quiet */
#else
	ZSTDStreamingCompressorState *cstate;
	MemoryContext oldcontext = MemoryContextSwitchTo(context);

	cstate = (ZSTDStreamingCompressorState *)
		MemoryContextAlloc(context, sizeof(ZSTDStreamingCompressorState));

	cstate->buf = makeStringInfo();

	/*
	 * We do not allocate ZSTD buffers and contexts at this point because we
	 * have no guarantee that we will need them later. Let's allocate only
	 * when we are about to use them.
	 */
	cstate->zstd_c_ctx = NULL;
	cstate->zstd_c_in_buf = NULL;
	cstate->zstd_c_in_buf_size = 0;
	cstate->zstd_c_out_buf = NULL;
	cstate->zstd_c_out_buf_size = 0;
	cstate->zstd_frame_size = 0;
	cstate->zstd_d_ctx = NULL;
	cstate->zstd_d_in_buf = NULL;
	cstate->zstd_d_in_buf_size = 0;
	cstate->zstd_d_out_buf = NULL;
	cstate->zstd_d_out_buf_size = 0;

	MemoryContextSwitchTo(oldcontext);

	return (void *) cstate;
#endif
}

/*
 * Free ZSTD memory resources and the compressor state.
 */
void
zstd_FreeCompressorState(MemoryContext context, void *compressor_state)
{
#ifndef USE_ZSTD
	NO_ZSTD_SUPPORT();
#else
	ZSTDStreamingCompressorState *cstate;
	MemoryContext oldcontext;

	if (compressor_state == NULL)
		return;

	oldcontext = MemoryContextSwitchTo(context);

	cstate = (ZSTDStreamingCompressorState *) compressor_state;

	destroyStringInfo(cstate->buf);

	if (cstate->zstd_c_ctx != NULL)
	{
		/* Compressor state was used for compression */
		pfree(cstate->zstd_c_in_buf);
		pfree(cstate->zstd_c_out_buf);
		ZSTD_freeCCtx(cstate->zstd_c_ctx);
	}
	if (cstate->zstd_d_ctx != NULL)
	{
		/* Compressor state was used for decompression */
		pfree(cstate->zstd_d_in_buf);
		pfree(cstate->zstd_d_out_buf);
		ZSTD_freeDCtx(cstate->zstd_d_ctx);
	}

	pfree(compressor_state);

	MemoryContextSwitchTo(oldcontext);
#endif
}

#ifdef USE_ZSTD
/*
 * Allocate ZSTD compression buffers and create the ZSTD compression context.
 */
static void
zstd_CreateStreamCompressorState(MemoryContext context, void *compressor_state)
{
	ZSTDStreamingCompressorState *cstate;
	MemoryContext oldcontext = MemoryContextSwitchTo(context);

	cstate = (ZSTDStreamingCompressorState *) compressor_state;
	cstate->zstd_c_in_buf_size = ZSTD_CStreamInSize();
	cstate->zstd_c_in_buf = (char *) palloc0(cstate->zstd_c_in_buf_size);
	cstate->zstd_c_out_buf_size = ZSTD_CStreamOutSize();
	cstate->zstd_c_out_buf = (char *) palloc0(cstate->zstd_c_out_buf_size);
	cstate->zstd_c_ctx = ZSTD_createCCtx();

	if (cstate->zstd_c_ctx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not create ZSTD compression context")));

	/* Set compression level */
	ZSTD_CCtx_setParameter(cstate->zstd_c_ctx, ZSTD_c_compressionLevel,
						   ZSTD_COMPRESSION_LEVEL);

	MemoryContextSwitchTo(oldcontext);
}
#endif

#ifdef USE_ZSTD
/*
 * Allocate ZSTD decompression buffers and create the ZSTD decompression
 * context.
 */
static void
zstd_CreateStreamDecodeCompressorState(MemoryContext context, void *compressor_state)
{
	ZSTDStreamingCompressorState *cstate;
	MemoryContext oldcontext = MemoryContextSwitchTo(context);

	cstate = (ZSTDStreamingCompressorState *) compressor_state;
	cstate->zstd_d_in_buf_size = ZSTD_DStreamInSize();
	cstate->zstd_d_in_buf = (char *) palloc0(cstate->zstd_d_in_buf_size);
	cstate->zstd_d_out_buf_size = ZSTD_DStreamOutSize();
	cstate->zstd_d_out_buf = (char *) palloc0(cstate->zstd_d_out_buf_size);
	cstate->zstd_d_ctx = ZSTD_createDCtx();

	if (cstate->zstd_d_ctx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not create ZSTD decompression context")));

	MemoryContextSwitchTo(oldcontext);
}
#endif

/*
 * Data compression using ZSTD streaming API.
 */
void
zstd_StreamingCompressData(MemoryContext context, char *src, Size src_size,
						   char *dst, Size *dst_size, void *compressor_state)
{
#ifndef USE_ZSTD
	NO_ZSTD_SUPPORT();
#else
	ZSTDStreamingCompressorState *cstate;

	/* Size of remaining data to be copied from src into ZSTD input buffer */
	Size		toCpy = src_size;
	char	   *dst_data;

	cstate = (ZSTDStreamingCompressorState *) compressor_state;
	/* Allocate ZSTD buffers and context */
	if (cstate->zstd_c_ctx == NULL)
		zstd_CreateStreamCompressorState(context, compressor_state);

	dst_data = dst;
	*dst_size = 0;

	/*
	 * ZSTD streaming compression works with chunks: the source data needs to
	 * be splitted out in chunks, each of them is then copied into ZSTD input
	 * buffer. For each chunk, we proceed with compression. Streaming
	 * compression is not intended to compress the whole input chunk, so we
	 * have the call ZSTD_compressStream2() multiple times until the entire
	 * chunk is consumed.
	 */
	while (toCpy > 0)
	{
		/* Are we on the last chunk? */
		bool		last_chunk = (toCpy < cstate->zstd_c_in_buf_size);

		/* Size of the data copied into ZSTD input buffer */
		Size		cpySize = last_chunk ? toCpy : cstate->zstd_c_in_buf_size;
		bool		finished = false;
		ZSTD_inBuffer input;
		ZSTD_EndDirective mode = last_chunk ? ZSTD_e_flush : ZSTD_e_continue;

		/* Copy data from src into ZSTD input buffer */
		memcpy(cstate->zstd_c_in_buf, src, cpySize);

		/*
		 * Close the frame when we are on the last chunk and we've reached max
		 * frame size.
		 */
		if (last_chunk && (cstate->zstd_frame_size > ZSTD_MAX_FRAME_SIZE))
		{
			mode = ZSTD_e_end;
			cstate->zstd_frame_size = 0;
		}

		cstate->zstd_frame_size += cpySize;

		input.src = cstate->zstd_c_in_buf;
		input.size = cpySize;
		input.pos = 0;

		do
		{
			Size		remaining;
			ZSTD_outBuffer output;

			output.dst = cstate->zstd_c_out_buf;
			output.size = cstate->zstd_c_out_buf_size;
			output.pos = 0;

			remaining = ZSTD_compressStream2(cstate->zstd_c_ctx, &output,
											 &input, mode);

			if (ZSTD_isError(remaining))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("ZSTD compression failed")));

			/* Copy back compressed data from ZSTD output buffer */
			memcpy(dst_data, (char *) cstate->zstd_c_out_buf, output.pos);

			dst_data += output.pos;
			*dst_size += output.pos;

			/*
			 * Compression is done when we are working on the last chunk and
			 * there is nothing left to compress, or, when we reach the end of
			 * the chunk.
			 */
			finished = last_chunk ? (remaining == 0) : (input.pos == input.size);
		} while (!finished);

		src += cpySize;
		toCpy -= cpySize;
	}
#endif
}

/*
 * Data decompression using ZSTD streaming API.
 */
void
zstd_StreamingDecompressData(MemoryContext context, char *src, Size src_size,
							 char *dst, Size dst_size, void *compressor_state)
{
#ifndef USE_ZSTD
	NO_ZSTD_SUPPORT();
#else
	ZSTDStreamingCompressorState *cstate;

	/* Size of remaining data to be copied from src into ZSTD input buffer */
	Size		toCpy = src_size;
	char	   *dst_data;
	Size		decBytes = 0;	/* Size of decompressed data */

	cstate = (ZSTDStreamingCompressorState *) compressor_state;
	/* Allocate ZSTD buffers and context */
	if (cstate->zstd_d_ctx == NULL)
		zstd_CreateStreamDecodeCompressorState(context, compressor_state);

	dst_data = dst;

	while (toCpy > 0)
	{
		ZSTD_inBuffer input;
		Size		cpySize = (toCpy > cstate->zstd_d_in_buf_size) ? cstate->zstd_d_in_buf_size : toCpy;

		/* Copy data from src into ZSTD input buffer */
		memcpy(cstate->zstd_d_in_buf, src, cpySize);

		input.src = cstate->zstd_d_in_buf;
		input.size = cpySize;
		input.pos = 0;

		while (input.pos < input.size)
		{
			ZSTD_outBuffer output;
			Size		ret;

			output.dst = cstate->zstd_d_out_buf;
			output.size = cstate->zstd_d_out_buf_size;
			output.pos = 0;

			ret = ZSTD_decompressStream(cstate->zstd_d_ctx, &output, &input);

			if (ZSTD_isError(ret))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("ZSTD decompression failed")));

			/* Copy back compressed data from ZSTD output buffer */
			memcpy(dst_data, (char *) cstate->zstd_d_out_buf, output.pos);

			dst_data += output.pos;
			decBytes += output.pos;
		}

		src += cpySize;
		toCpy -= cpySize;
	}

	Assert(dst_size == decBytes);
#endif
}

Size
zstd_CompressBound(Size src_size)
{
#ifndef USE_ZSTD
	NO_ZSTD_SUPPORT();
	return -1;
#else
	return ZSTD_compressBound(src_size);
#endif
}

/*
 * Returns the StringInfo buffer we use to store compressed/decompressed data.
 */
StringInfo
zstd_GetStringInfoBuffer(void *compressor_state)
{
#ifndef USE_ZSTD
	NO_ZSTD_SUPPORT();
	return NULL;
#else
	ZSTDStreamingCompressorState *cstate;

	cstate = (ZSTDStreamingCompressorState *) compressor_state;

	return cstate->buf;
#endif
}
