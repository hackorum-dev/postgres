/*-------------------------------------------------------------------------
 *
 * memutils_generichdr.h
 *	  Contains various inlined functions which can be used by implementations
 *	  of MemoryContexts to use as a memory chunk header.  Implementations may
 *	  choose to implement their own header.  The only requirement is that the
 *	  3-bits directly prior to the pointer must be set to the
 *	  MemoryContextMethodID of the given context.
 *
 * This generic chunk header allows the encoding of various details about the
 * memory chunk.  Generally, these headers are 8 bytes in length, however for
 * larger allocations 24 bytes are required.  We refer to the former as
 * "small" headers and the latter as "large" headers.
 *
 * Both small and large headers have a 8-byte portion directly prior to the
 * memory pointer for the chunk.  We encode 4 separate pieces of information
 * into these 8 bytes. These are as follows in order of least significant bit
 * first:
 *
 * 1. 3-bits to indicate the MemoryContextMethodID
 * 2. 1-bit to indicate if the chunk is small or large
 * 3. 30-bits to indicate the size of the chunk
 * 4. 30-bits to indicate the number of bytes that must be subtracted from the
 *    pointer to obtain the address of the block that the pointer is stored on
 *
 * Memory allocations where the size of the chunk or the block offset is a
 * value that consumes more than 30-bits (1GB) must use a large chunk.
 *
 * Large chunks, as mentioned above, also contain a 8-byte header, however
 * the 30-bit portions #3 and #4 above are unused.  Instead the chunk size and
 * block offset are stored directly prior to the 8-byte chunk header in the
 * following form:
 *
 *	<Size block offset><Size chunk size><8 byte header>
 *
 * Which is 24 bytes when sizeof(Size) is 8 and 16 bytes when sizeof(Size) is
 * 4.
 *
 * When MEMORY_CONTEXT_CHECKING is defined, both small and large headers store
 * an additional Size variable which directly prefixes the existing chunk.
 * This additional Size field is used to store the requested size of the
 * allocation. This can be used by the MemoryContexts implementation to
 * perform additional checks to help ensure the code is correct. For example,
 * setting sentinal bytes to help ensure that no code overruns the memory
 * allocation.
 *
 * Interface:
 *	GenericChunkHeaderSize:
 *		Used to predetermine the size of the chunk.
 *
 *	GenericChunkHeaderDecode:
 *		Given a pointer to the memory, determine the size of the chunk and
 *		the number of bytes to subtract from the pointer to obtain the pointer
 *		to the block.  The requested_size is also decoded when
 *		MEMORY_CONTEXT_CHECKING is defined.
 *
 * GenericChunkHeaderEncode:
 *		Given a pointer to the memory and a pointer to the block, determine
 *		the bytes offset for the block and encode that and the
 *		MemoryContextMethodID into the chunk. Also encode the chunk size and
 *		optionally, the requested_size when MEMORY_CONTEXT_CHECKING is
 *		defined.
 *
 * Also exports:
 *		SMALL_CHUNK_LIMIT
 *		SMALL_CHUNK_SIZE
 *		LARGE_CHUNK_SIZE
 *		
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/memutils_generichdr.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MEMUTILS_GENERICHDR_H
#define MEMUTILS_GENERICHDR_H

#include "utils/memutils_internal.h"

/*
 * The threshold, above which a chunk becomes large.  The maximum value that
 * can be stored in 30-bits.
 */
#define SMALL_CHUNK_LIMIT 0x3FFFFFFF

/*
 * Bit mask with 30 of the least significant bits on.  Generally the same as
 * SMALL_CHUNK_LIMIT, but kept separate so that SMALL_CHUNK_LIMIT can be
 * lowered to make testing large chunks easier.
 */
#define SMALL_CHUNK_SIZE_MASK 0x3FFFFFFF

/*
 * The value to AND onto the 8-byte header to determine if it's a large chunk.
 */
#define LARGE_CHUNK_MASK (1 << 3)

#ifndef MEMORY_CONTEXT_CHECKING
#define SMALL_CHUNK_SIZE sizeof(uint64)
#define LARGE_CHUNK_SIZE (sizeof(uint64) + (sizeof(Size) * 2))
#else
#define SMALL_CHUNK_SIZE (sizeof(uint64) + sizeof(Size))
#define LARGE_CHUNK_SIZE (sizeof(uint64) + (sizeof(Size) * 3))
#endif

#define LargeChunkGetChunkSizePtr(p)   (Size *) (((char *) (p)) - sizeof(uint64) - sizeof(Size))
#define LargeChunkGetBlockOffsetPtr(p) (Size *) (((char *) (p)) - sizeof(uint64) - (sizeof(Size) * 2))

#ifdef MEMORY_CONTEXT_CHECKING
#define LargeChunkGetRequestedSizePtr(p) (Size *) (((char *) (p)) - sizeof(uint64) - (sizeof(Size) * 3))
#endif

/* Get a pointer to the uint64 header for either a small or large chunk */
#define ChunkGetHeaderPtr(p) ((uint64 *) (((char *) p) - sizeof(uint64)))

/*
 * From a small or large chunk's header uint64 value, get the context method id
 */
#define ChunkHdrGetMethodId(h) ((h) & 7)

#define SmallChunkHdrGetChunkSize(h) (((h) >> 4) & SMALL_CHUNK_SIZE_MASK)
#define SmallChunkHdrGetBlockOffset(h) (((h) >> 34) & SMALL_CHUNK_SIZE_MASK)

#ifdef MEMORY_CONTEXT_CHECKING
#define SmallChunkGetRequestedSizePtr(p)   (Size *) (((char *) (p)) - sizeof(uint64) - sizeof(Size))
#endif

 /*
  * Determine if offset or size justify requiring a large chunk.  Since
  * SMALL_CHUNK_LIMIT is a power of 2, we can bitmask OR offset and size to
  * get a slightly more efficient way of checking if either is larger than
  * SMALL_CHUNK_LIMIT.
  */
#define LargeChunkRequired(offset, size) (((offset) | (size)) > SMALL_CHUNK_LIMIT)

  /* From a given chunk header, return true if it's a large chunk */
#define IsLargeChunk(h) ((h) & LARGE_CHUNK_MASK)

/*
 * GenericChunkHeaderSize
 *		Return the size of chunk header required to store details about a
 *		chunk with the given blockoffset and chunkisize.
 */
static inline Size
GenericChunkHeaderSize(Size blockoffset, Size chunksize)
{
	return LargeChunkRequired(blockoffset, chunksize) ? LARGE_CHUNK_SIZE : SMALL_CHUNK_SIZE;
}

/*
 * GenericChunkHeaderDecode
 *		Decode the generic chunk header preceeding 'pointer' and populate
 *		'chunksize' and 'blockoffset'.  Return the size of the generic header.
 */
static inline Size
#ifdef MEMORY_CONTEXT_CHECKING
GenericChunkHeaderDecode(void *pointer, Size *chunksize, Size *blockoffset,
	Size *requested_size)
#else
GenericChunkHeaderDecode(void *pointer, Size *chunksize, Size *blockoffset)
#endif
{
	uint64	header = (uint64) *ChunkGetHeaderPtr(pointer);

	if (unlikely(IsLargeChunk(header)))
	{
		*chunksize = (Size) *LargeChunkGetChunkSizePtr(pointer);
		*blockoffset = (Size) *LargeChunkGetBlockOffsetPtr(pointer);

#ifdef MEMORY_CONTEXT_CHECKING
		*requested_size = (Size) *LargeChunkGetRequestedSizePtr(pointer);
#endif
		return LARGE_CHUNK_SIZE;
	}
	else
	{
		*chunksize = SmallChunkHdrGetChunkSize(header);
		*blockoffset = SmallChunkHdrGetBlockOffset(header);
#ifdef MEMORY_CONTEXT_CHECKING
		*requested_size = (Size) *SmallChunkGetRequestedSizePtr(pointer);
#endif

		return SMALL_CHUNK_SIZE;
	}
}

static inline void
#ifdef MEMORY_CONTEXT_CHECKING
GenericChunkHeaderEncode(void *pointer, void *block, Size chunksize,
						 Size requested_size, MemoryContextMethodID methodid)
#else
GenericChunkHeaderEncode(void *pointer, void *block, Size chunksize,
						 MemoryContextMethodID methodid)
#endif
{
	uint64	   *p_header = ChunkGetHeaderPtr(pointer);
	Size		blockoffset = (char *) pointer - (char *) block;

	/* Check if we need a large chunk */
	if (unlikely(LargeChunkRequired(blockoffset, chunksize)))
	{
		Size	*p_chunksize = LargeChunkGetChunkSizePtr(pointer);
		Size	*p_blockoffset = LargeChunkGetBlockOffsetPtr(pointer);
#ifdef MEMORY_CONTEXT_CHECKING
		Size	*p_requestedsize = LargeChunkGetRequestedSizePtr(pointer);
#endif

		/* Encode the 8-byte header */
		*p_header = LARGE_CHUNK_MASK | methodid;

		/* Set the chunk size and block offset */
		*p_chunksize = chunksize;

		*p_blockoffset = blockoffset;

#ifdef MEMORY_CONTEXT_CHECKING
		*p_requestedsize = requested_size;
#endif
	}
	else
	{
		*p_header = (blockoffset << 34) | (chunksize << 4) | methodid;
#ifdef MEMORY_CONTEXT_CHECKING
		*SmallChunkGetRequestedSizePtr(pointer) = requested_size;
#endif
	}

#ifdef USE_ASSERT_CHECKING
	/* verify that we decode the values the same as we've just encoded them */
	{
		Size chsz, blkoff;
#ifdef MEMORY_CONTEXT_CHECKING
		Size reqsz;

		GenericChunkHeaderDecode(pointer, &chsz, &blkoff, &reqsz);

		Assert(reqsz == requested_size);
#else
		GenericChunkHeaderDecode(pointer, &chsz, &blkoff);
#endif
		Assert(chsz == chunksize);
		Assert(blkoff == blockoffset);
		Assert(blkoff > 0);
		Assert(GetMemoryChunkMethodID(pointer) == methodid);
	}
#endif

}

/* cleanup all internal definitions */
#undef LARGE_CHUNK_MASK
#undef LargeChunkGetChunkSizePtr
#undef LargeChunkGetBlockOffsetPtr
#undef LargeChunkGetRequestedSizePtr
#undef ChunkGetHeaderPtr
#undef ChunkHdrGetMethodId
#undef SmallChunkHdrGetChunkSize
#undef SmallChunkHdrGetBlockOffset
#undef SmallChunkGetRequestedSizePtr
#undef LargeChunkRequired
#undef IsLargeChunk

#endif							/* MEMUTILS_GENERICHDR_H */
