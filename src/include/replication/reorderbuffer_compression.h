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

#ifdef USE_LZ4
#include <lz4.h>
#endif

#ifdef USE_ZSTD
#include <zstd.h>
#endif

/* ReorderBuffer on disk compression algorithms */
typedef enum ReorderBufferCompressionMethod
{
	REORDER_BUFFER_NO_COMPRESSION,
	REORDER_BUFFER_PGLZ_COMPRESSION,
	REORDER_BUFFER_LZ4_COMPRESSION,
	REORDER_BUFFER_ZSTD_COMPRESSION,
}			ReorderBufferCompressionMethod;

/*
 * Compression strategy applied to ReorderBuffer records spilled on disk
 */
typedef enum ReorderBufferCompressionStrategy
{
	REORDER_BUFFER_STRAT_UNCOMPRESSED,
	REORDER_BUFFER_STRAT_PGLZ,
	REORDER_BUFFER_STRAT_LZ4_STREAMING,
	REORDER_BUFFER_STRAT_ZSTD_STREAMING,
}			ReorderBufferCompressionStrategy;

typedef struct PGLZCompressorState
{
	/* Buffer used to store compressed data */
	StringInfo	buf;
}			PGLZCompressorState;

#ifdef USE_LZ4
/*
 * We use a fairly small LZ4 ring buffer size (64kB). Using a larger buffer
 * size provide better compression ratio, but as long as we have to allocate
 * two LZ4 ring buffers per ReorderBufferTXN, we should keep it small.

 * 64kB is also twice the maximum size of a block, which is enough to cover
 * changes like UPDATE that will contain data of the old and new version of a
 * tuple.
 */
#define LZ4_RING_BUFFER_SIZE (64 * 1024)

/*
 * LZ4 streaming compression/decompression contextes and buffers.
 */
typedef struct LZ4StreamingCompressorState
{
	/* Streaming compression handler */
	LZ4_stream_t *lz4_stream;
	/* Streaming decompression handler */
	LZ4_streamDecode_t *lz4_stream_decode;
	/* LZ4 in/out ring buffers used for streaming compression */
	char	   *lz4_in_buf;
	int			lz4_in_buf_offset;
	char	   *lz4_out_buf;
	int			lz4_out_buf_offset;
	/* Buffer used to store compressed data */
	StringInfo	buf;
}			LZ4StreamingCompressorState;
#endif

#ifdef USE_ZSTD
/*
 * Low compression level provides high compression speed and decent compression
 * rate. Minimum level is 1, maximum is 22.
 */
#define ZSTD_COMPRESSION_LEVEL 1

/*
 * Maximum volume of data encoded in the current ZSTD frame. When this
 * threshold is reached then we close the current frame and start a new one.
 */
#define ZSTD_MAX_FRAME_SIZE (64 * 1024)

/*
 * ZSTD streaming compression/decompression handlers and buffers.
 */
typedef struct ZSTDStreamingCompressorState
{
	/* Compression */
	ZSTD_CCtx  *zstd_c_ctx;
	Size		zstd_c_in_buf_size;
	char	   *zstd_c_in_buf;
	Size		zstd_c_out_buf_size;
	char	   *zstd_c_out_buf;
	Size		zstd_frame_size;
	/* Decompression */
	ZSTD_DCtx  *zstd_d_ctx;
	Size		zstd_d_in_buf_size;
	char	   *zstd_d_in_buf;
	Size		zstd_d_out_buf_size;
	char	   *zstd_d_out_buf;
	/* Buffer used to store compressed/decompressed data */
	StringInfo	buf;
}			ZSTDStreamingCompressorState;
#endif

extern void *lz4_NewCompressorState(MemoryContext context);
extern void lz4_FreeCompressorState(MemoryContext context,
									void *compressor_state);
extern void lz4_StreamingCompressData(MemoryContext context, char *src,
									  Size src_size, char *dst, Size *dst_size,
									  void *compressor_state);
extern void lz4_StreamingDecompressData(MemoryContext context, char *src,
										Size src_size, char **dst,
										Size dst_size, void *compressor_state);
extern Size lz4_CompressBound(Size src_size);
extern StringInfo lz4_GetStringInfoBuffer(void *compressor_state);

extern void *pglz_NewCompressorState(MemoryContext context);
extern void pglz_FreeCompressorState(MemoryContext context,
									 void *compressor_state);

extern void *zstd_NewCompressorState(MemoryContext context);
extern void zstd_FreeCompressorState(MemoryContext context,
									 void *compressor_state);
extern void zstd_StreamingCompressData(MemoryContext context, char *src,
									   Size src_size, char *dst, Size *dst_size,
									   void *compressor_state);
extern void zstd_StreamingDecompressData(MemoryContext context, char *src,
										 Size src_size, char *dst,
										 Size dst_size, void *compressor_state);
extern Size zstd_CompressBound(Size src_size);
extern StringInfo zstd_GetStringInfoBuffer(void *compressor_state);

#endif							/* REORDERBUFFER_COMPRESSION_H */
