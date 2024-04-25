/*-------------------------------------------------------------------------
 *
 * write_stream.h
 *	  Mechanism for writing out buffered data efficiently
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/write_stream.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WRITE_STREAM_H
#define WRITE_STREAM_H

#include "storage/bufmgr.h"

/*
 * An opaque handle type returned by write_stream_write_buffer.  Calls can
 * optionally wait for individual buffers to be written using this handle.
 */
typedef struct WriteStreamWriteHandle
{
	uint64	   *p;
	uint64		c;
} WriteStreamWriteHandle;

struct WriteStream;
typedef struct WriteStream WriteStream;

extern WriteStream *write_stream_begin(int flags,
									   struct WritebackContext *wb_context,
									   int max_deferred_writes);
extern WriteStreamWriteHandle write_stream_write_buffer(WriteStream *stream,
														Buffer buffer);
extern void write_stream_wait_slow(WriteStream *stream,
								   WriteStreamWriteHandle handle);
extern void write_stream_wait_all(WriteStream *stream);
extern void write_stream_reset(WriteStream *stream);
extern void write_stream_end(WriteStream *stream);

/*
 * Wait for an individual write_stream_write_buffer() operation to be
 * finished.  While write_stream_end() finished all writes, some callers might
 * need to wait for an individual buffer write to complete without waiting for
 * all of them.
 */
static inline void
write_stream_wait(WriteStream *stream, WriteStreamWriteHandle handle)
{
	/*
	 * Only bother to enter the slow path if the completion counter hasn't
	 * moved.  Tolerate zero-initialized object.
	 */
	if (handle.p && *handle.p == handle.c)
		write_stream_wait_slow(stream, handle);
}

#endif
