/*-------------------------------------------------------------------------
 *
 * aset.c
 *	  Allocation set definitions.
 *
 * AllocSet is our standard implementation of the abstract MemoryContext
 * type.
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/aset.c
 *
 * NOTE:
 *	This is a new (Feb. 05, 1999) implementation of the allocation set
 *	routines. AllocSet...() does not use OrderedSet...() any more.
 *	Instead it manages allocations in a block pool by itself, combining
 *	many small allocations in a few bigger blocks. AllocSetFree() normally
 *	doesn't free() memory really. It just add's the free'd area to some
 *	list for later reuse by AllocSetAlloc(). All memory blocks are free()'d
 *	at once on AllocSetReset(), which happens when the memory context gets
 *	destroyed.
 *				Jan Wieck
 *
 *	Performance improvement from Tom Lane, 8/99: for extremely large request
 *	sizes, we do want to be able to give the memory back to free() as soon
 *	as it is pfree()'d.  Otherwise we risk tying up a lot of memory in
 *	freelist entries that might never be usable.  This is specially needed
 *	when the caller is repeatedly repalloc()'ing a block bigger and bigger;
 *	the previous instances of the block were guaranteed to be wasted until
 *	AllocSetReset() under the old way.
 *
 *	Further improvement 12/00: as the code stood, request sizes in the
 *	midrange between "small" and "large" were handled very inefficiently,
 *	because any sufficiently large free chunk would be used to satisfy a
 *	request, even if it was much larger than necessary.  This led to more
 *	and more wasted space in allocated chunks over time.  To fix, get rid
 *	of the midrange behavior: we now handle only "small" power-of-2-size
 *	chunks as chunks.  Anything "large" is passed off to malloc().  Change
 *	the number of freelists to change the small/large boundary.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/memutils_generichdr.h"
#include "utils/memutils_internal.h"

/*--------------------
 * Chunk freelist k holds chunks of size 1 << (k + ALLOC_MINBITS),
 * for k = 0 .. ALLOCSET_NUM_FREELISTS-1.
 *
 * Note that all chunks in the freelists have power-of-2 sizes.  This
 * improves recyclability: we may waste some space, but the wasted space
 * should stay pretty constant as requests are made and released.
 *
 * A request too large for the last freelist is handled by allocating a
 * dedicated block from malloc().  The block still has a block header and
 * chunk header, but when the chunk is freed we'll return the whole block
 * to malloc(), not put it on our freelists.
 *
 * CAUTION: ALLOC_MINBITS must be large enough so that
 * 1<<ALLOC_MINBITS is at least MAXALIGN,
 * or we may fail to align the smallest chunks adequately.
 * 8-byte alignment is enough on all currently known machines.  This 8-byte
 * minimum also allows us to store a pointer to the next freelist item within
 * the chunk of memory itself.
 *
 * With the current parameters, request sizes up to 8K are treated as chunks,
 * larger requests go into dedicated blocks.  Change ALLOCSET_NUM_FREELISTS
 * to adjust the boundary point; and adjust ALLOCSET_SEPARATE_THRESHOLD in
 * memutils.h to agree.  (Note: in contexts with small maxBlockSize, we may
 * set the allocChunkLimit to less than 8K, so as to avoid space wastage.)
 *--------------------
 */

#define ALLOC_MINBITS		3	/* smallest chunk size is 8 bytes */
#define ALLOCSET_NUM_FREELISTS	11
#define ALLOC_CHUNK_LIMIT	(1 << (ALLOCSET_NUM_FREELISTS-1+ALLOC_MINBITS))
/* Size of largest chunk that we use a fixed size for */
#define ALLOC_CHUNK_FRACTION	4
/* We allow chunks to be at most 1/4 of maxBlockSize (less overhead) */

/*--------------------
 * The first block allocated for an allocset has size initBlockSize.
 * Each time we have to allocate another block, we double the block size
 * (if possible, and without exceeding maxBlockSize), so as to reduce
 * the bookkeeping load on malloc().
 *
 * Blocks allocated to hold oversize chunks do not follow this rule, however;
 * they are just however big they need to be to hold that single chunk.
 *
 * Also, if a minContextSize is specified, the first block has that size,
 * and then initBlockSize is used for the next one.
 *--------------------
 */

#define ALLOC_BLOCKHDRSZ	MAXALIGN(sizeof(AllocBlockData))

typedef struct AllocBlockData *AllocBlock;	/* forward reference */

/*
 * AllocPointer
 *		Aligned pointer which may be a member of an allocation set.
 */
typedef void *AllocPointer;

/*
 * AllocSetContext is our standard implementation of MemoryContext.
 *
 * Note: header.isReset means there is nothing for AllocSetReset to do.
 * This is different from the aset being physically empty (empty blocks list)
 * because we will still have a keeper block.  It's also different from the set
 * being logically empty, because we don't attempt to detect pfree'ing the
 * last active chunk.
 */
typedef struct AllocSetContext
{
	MemoryContextData header;	/* Standard memory-context fields */
	/* Info about storage allocated in this context: */
	AllocBlock	blocks;			/* head of list of blocks in this set */
	void	   *freelist[ALLOCSET_NUM_FREELISTS];	/* free pointer lists */
	/* Allocation parameters for this context: */
	Size		initBlockSize;	/* initial block size */
	Size		maxBlockSize;	/* maximum block size */
	Size		nextBlockSize;	/* next block size to allocate */
	Size		allocChunkLimit;	/* effective chunk size limit */
	AllocBlock	keeper;			/* keep this block over resets */
	/* freelist this context could be put in, or -1 if not a candidate: */
	int			freeListIndex;	/* index in context_freelists[], or -1 */
} AllocSetContext;

typedef AllocSetContext *AllocSet;

/*
 * AllocBlock
 *		An AllocBlock is the unit of memory that is obtained by aset.c
 *		from malloc().  It contains one or more allocated chunks of memory,
 *		which are the units requested by palloc() and freed by pfree().
 *		Chunks allocated by palloc cannot be returned to malloc()
 *		individually, instead they are put on freelists by pfree() and re-used
 *		by the next palloc() that has a matching chunk size.
 *
 *		AllocBlockData is the header data for a block --- the usable space
 *		within the block begins at the next alignment boundary.
 */
typedef struct AllocBlockData
{
	AllocSet	aset;			/* aset that owns this block */
	AllocBlock	prev;			/* prev block in aset's blocks list, if any */
	AllocBlock	next;			/* next block in aset's blocks list, if any */
	bool		large_chunk;	/* true if the only chunk is a large chunk */
	char	   *freeptr;		/* start of free space in this block */
	char	   *endptr;			/* end of space in this block */
}			AllocBlockData;

/*
 * AllocPointerIsValid
 *		True iff pointer is valid allocation pointer.
 */
#define AllocPointerIsValid(pointer) PointerIsValid(pointer)

/*
 * AllocSetIsValid
 *		True iff set is valid allocation set.
 */
#define AllocSetIsValid(set) PointerIsValid(set)

/*
 * Rather than repeatedly creating and deleting memory contexts, we keep some
 * freed contexts in freelists so that we can hand them out again with little
 * work.  Before putting a context in a freelist, we reset it so that it has
 * only its initial malloc chunk and no others.  To be a candidate for a
 * freelist, a context must have the same minContextSize/initBlockSize as
 * other contexts in the list; but its maxBlockSize is irrelevant since that
 * doesn't affect the size of the initial chunk.
 *
 * We currently provide one freelist for ALLOCSET_DEFAULT_SIZES contexts
 * and one for ALLOCSET_SMALL_SIZES contexts; the latter works for
 * ALLOCSET_START_SMALL_SIZES too, since only the maxBlockSize differs.
 *
 * Ordinarily, we re-use freelist contexts in last-in-first-out order, in
 * hopes of improving locality of reference.  But if there get to be too
 * many contexts in the list, we'd prefer to drop the most-recently-created
 * contexts in hopes of keeping the process memory map compact.
 * We approximate that by simply deleting all existing entries when the list
 * overflows, on the assumption that queries that allocate a lot of contexts
 * will probably free them in more or less reverse order of allocation.
 *
 * Contexts in a freelist are chained via their nextchild pointers.
 */
#define MAX_FREE_CONTEXTS 100	/* arbitrary limit on freelist length */

typedef struct AllocSetFreeList
{
	int			num_free;		/* current list length */
	AllocSetContext *first_free;	/* list header */
} AllocSetFreeList;

/* context_freelists[0] is for default params, [1] for small params */
static AllocSetFreeList context_freelists[2] =
{
	{
		0, NULL
	},
	{
		0, NULL
	}
};


/* ----------
 * AllocSetFreeIndex -
 *
 *		Depending on the size of an allocation compute which freechunk
 *		list of the alloc set it belongs to.  Caller must have verified
 *		that size <= ALLOC_CHUNK_LIMIT.
 * ----------
 */
static inline int
AllocSetFreeIndex(Size size)
{
	int			idx;

	if (size > (1 << ALLOC_MINBITS))
	{
		/*----------
		 * At this point we must compute ceil(log2(size >> ALLOC_MINBITS)).
		 * This is the same as
		 *		pg_leftmost_one_pos32((size - 1) >> ALLOC_MINBITS) + 1
		 * or equivalently
		 *		pg_leftmost_one_pos32(size - 1) - ALLOC_MINBITS + 1
		 *
		 * However, rather than just calling that function, we duplicate the
		 * logic here, allowing an additional optimization.  It's reasonable
		 * to assume that ALLOC_CHUNK_LIMIT fits in 16 bits, so we can unroll
		 * the byte-at-a-time loop in pg_leftmost_one_pos32 and just handle
		 * the last two bytes.
		 *
		 * Yes, this function is enough of a hot-spot to make it worth this
		 * much trouble.
		 *----------
		 */
#ifdef HAVE__BUILTIN_CLZ
		idx = 31 - __builtin_clz((uint32) size - 1) - ALLOC_MINBITS + 1;
#else
		uint32		t,
					tsize;

		/* Statically assert that we only have a 16-bit input value. */
		StaticAssertStmt(ALLOC_CHUNK_LIMIT < (1 << 16),
						 "ALLOC_CHUNK_LIMIT must be less than 64kB");

		tsize = size - 1;
		t = tsize >> 8;
		idx = t ? pg_leftmost_one_pos[t] + 8 : pg_leftmost_one_pos[tsize];
		idx -= ALLOC_MINBITS - 1;
#endif

		Assert(idx < ALLOCSET_NUM_FREELISTS);
	}
	else
		idx = 0;

	return idx;
}


/*
 * Public routines
 */


/*
 * AllocSetContextCreateInternal
 *		Create a new AllocSet context.
 *
 * parent: parent context, or NULL if top-level context
 * name: name of context (must be statically allocated)
 * minContextSize: minimum context size
 * initBlockSize: initial allocation block size
 * maxBlockSize: maximum allocation block size
 *
 * Most callers should abstract the context size parameters using a macro
 * such as ALLOCSET_DEFAULT_SIZES.
 *
 * Note: don't call this directly; go through the wrapper macro
 * AllocSetContextCreate.
 */
MemoryContext
AllocSetContextCreateInternal(MemoryContext parent,
							  const char *name,
							  Size minContextSize,
							  Size initBlockSize,
							  Size maxBlockSize)
{
	int			freeListIndex;
	Size		firstBlockSize;
	AllocSet	set;
	AllocBlock	block;

	/*
	 * First, validate allocation parameters.  Once these were regular runtime
	 * test and elog's, but in practice Asserts seem sufficient because nobody
	 * varies their parameters at runtime.  We somewhat arbitrarily enforce a
	 * minimum 1K block size.
	 */
	Assert(initBlockSize == MAXALIGN(initBlockSize) &&
		   initBlockSize >= 1024);
	Assert(maxBlockSize == MAXALIGN(maxBlockSize) &&
		   maxBlockSize >= initBlockSize &&
		   AllocHugeSizeIsValid(maxBlockSize)); /* must be safe to double */
	Assert(minContextSize == 0 ||
		   (minContextSize == MAXALIGN(minContextSize) &&
			minContextSize >= 1024 &&
			minContextSize <= maxBlockSize));
	Assert(maxBlockSize <= SMALL_CHUNK_LIMIT);

	/*
	 * Check whether the parameters match either available freelist.  We do
	 * not need to demand a match of maxBlockSize.
	 */
	if (minContextSize == ALLOCSET_DEFAULT_MINSIZE &&
		initBlockSize == ALLOCSET_DEFAULT_INITSIZE)
		freeListIndex = 0;
	else if (minContextSize == ALLOCSET_SMALL_MINSIZE &&
			 initBlockSize == ALLOCSET_SMALL_INITSIZE)
		freeListIndex = 1;
	else
		freeListIndex = -1;

	/*
	 * If a suitable freelist entry exists, just recycle that context.
	 */
	if (freeListIndex >= 0)
	{
		AllocSetFreeList *freelist = &context_freelists[freeListIndex];

		if (freelist->first_free != NULL)
		{
			/* Remove entry from freelist */
			set = freelist->first_free;
			freelist->first_free = (AllocSet) set->header.nextchild;
			freelist->num_free--;

			/* Update its maxBlockSize; everything else should be OK */
			set->maxBlockSize = maxBlockSize;

			/* Reinitialize its header, installing correct name and parent */
			MemoryContextCreate((MemoryContext) set,
								T_AllocSetContext,
								MCTX_ASET_ID,
								parent,
								name);

			((MemoryContext) set)->mem_allocated =
				set->keeper->endptr - ((char *) set);

			return (MemoryContext) set;
		}
	}

	/* Determine size of initial block */
	firstBlockSize = MAXALIGN(sizeof(AllocSetContext)) +
		ALLOC_BLOCKHDRSZ + SMALL_CHUNK_SIZE;
	if (minContextSize != 0)
		firstBlockSize = Max(firstBlockSize, minContextSize);
	else
		firstBlockSize = Max(firstBlockSize, initBlockSize);

	/*
	 * Allocate the initial block.  Unlike other aset.c blocks, it starts with
	 * the context header and its block header follows that.
	 */
	set = (AllocSet) malloc(firstBlockSize);
	if (set == NULL)
	{
		if (TopMemoryContext)
			MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while creating memory context \"%s\".",
						   name)));
	}

	/*
	 * Avoid writing code that can fail between here and MemoryContextCreate;
	 * we'd leak the header/initial block if we ereport in this stretch.
	 */

	/* Fill in the initial block's block header */
	block = (AllocBlock) (((char *) set) + MAXALIGN(sizeof(AllocSetContext)));
	block->aset = set;
	block->large_chunk = false;
	block->freeptr = ((char *) block) + ALLOC_BLOCKHDRSZ;
	block->endptr = ((char *) set) + firstBlockSize;
	block->prev = NULL;
	block->next = NULL;

	/* Mark unallocated space NOACCESS; leave the block header alone. */
	VALGRIND_MAKE_MEM_NOACCESS(block->freeptr, block->endptr - block->freeptr);

	/* Remember block as part of block list */
	set->blocks = block;
	/* Mark block as not to be released at reset time */
	set->keeper = block;

	/* Finish filling in aset-specific parts of the context header */
	MemSetAligned(set->freelist, 0, sizeof(set->freelist));

	set->initBlockSize = initBlockSize;
	set->maxBlockSize = maxBlockSize;
	set->nextBlockSize = initBlockSize;
	set->freeListIndex = freeListIndex;

	/*
	 * Compute the allocation chunk size limit for this context.  It can't be
	 * more than ALLOC_CHUNK_LIMIT because of the fixed number of freelists.
	 * If maxBlockSize is small then requests exceeding the maxBlockSize, or
	 * even a significant fraction of it, should be treated as large chunks
	 * too.  For the typical case of maxBlockSize a power of 2, the chunk size
	 * limit will be at most 1/8th maxBlockSize, so that given a stream of
	 * requests that are all the maximum chunk size we will waste at most
	 * 1/8th of the allocated space.
	 *
	 * We have to have allocChunkLimit a power of two, because the requested
	 * and actually-allocated sizes of any chunk must be on the same side of
	 * the limit, else we get confused about whether the chunk is large.
	 *
	 * Also, allocChunkLimit must not exceed ALLOCSET_SEPARATE_THRESHOLD.
	 */
	StaticAssertStmt(ALLOC_CHUNK_LIMIT == ALLOCSET_SEPARATE_THRESHOLD,
					 "ALLOC_CHUNK_LIMIT != ALLOCSET_SEPARATE_THRESHOLD");

	/* We always want to allocate non-dedicated blocks as small chunks */
	set->allocChunkLimit = Min(ALLOC_CHUNK_LIMIT, SMALL_CHUNK_LIMIT);
	while ((Size) (set->allocChunkLimit + SMALL_CHUNK_SIZE) >
		   (Size) ((maxBlockSize - ALLOC_BLOCKHDRSZ) / ALLOC_CHUNK_FRACTION))
		set->allocChunkLimit >>= 1;

	/* Finally, do the type-independent part of context creation */
	MemoryContextCreate((MemoryContext) set,
						T_AllocSetContext,
						MCTX_ASET_ID,
						parent,
						name);

	((MemoryContext) set)->mem_allocated = firstBlockSize;

	return (MemoryContext) set;
}

/*
 * AllocSetReset
 *		Frees all memory which is allocated in the given set.
 *
 * Actually, this routine has some discretion about what to do.
 * It should mark all allocated chunks freed, but it need not necessarily
 * give back all the resources the set owns.  Our actual implementation is
 * that we give back all but the "keeper" block (which we must keep, since
 * it shares a malloc chunk with the context header).  In this way, we don't
 * thrash malloc() when a context is repeatedly reset after small allocations,
 * which is typical behavior for per-tuple contexts.
 */
void
AllocSetReset(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block;
	Size		keepersize PG_USED_FOR_ASSERTS_ONLY
	= set->keeper->endptr - ((char *) set);

	AssertArg(AllocSetIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* Check for corruption and leaks before freeing */
	AllocSetCheck(context);
#endif

	/* Clear chunk freelists */
	MemSetAligned(set->freelist, 0, sizeof(set->freelist));

	block = set->blocks;

	/* New blocks list will be just the keeper block */
	set->blocks = set->keeper;

	while (block != NULL)
	{
		AllocBlock	next = block->next;

		if (block == set->keeper)
		{
			/* Reset the block, but don't return it to malloc */
			char	   *datastart = ((char *) block) + ALLOC_BLOCKHDRSZ;

#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(datastart, block->freeptr - datastart);
#else
			/* wipe_mem() would have done this */
			VALGRIND_MAKE_MEM_NOACCESS(datastart, block->freeptr - datastart);
#endif
			block->freeptr = datastart;
			block->prev = NULL;
			block->next = NULL;
		}
		else
		{
			/* Normal case, release the block */
			context->mem_allocated -= block->endptr - ((char *) block);

#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(block, block->freeptr - ((char *) block));
#endif
			free(block);
		}
		block = next;
	}

	Assert(context->mem_allocated == keepersize);

	/* Reset block size allocation sequence, too */
	set->nextBlockSize = set->initBlockSize;
}

/*
 * AllocSetDelete
 *		Frees all memory which is allocated in the given set,
 *		in preparation for deletion of the set.
 *
 * Unlike AllocSetReset, this *must* free all resources of the set.
 */
void
AllocSetDelete(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block = set->blocks;
	Size		keepersize PG_USED_FOR_ASSERTS_ONLY
	= set->keeper->endptr - ((char *) set);

	AssertArg(AllocSetIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* Check for corruption and leaks before freeing */
	AllocSetCheck(context);
#endif

	/*
	 * If the context is a candidate for a freelist, put it into that freelist
	 * instead of destroying it.
	 */
	if (set->freeListIndex >= 0)
	{
		AllocSetFreeList *freelist = &context_freelists[set->freeListIndex];

		/*
		 * Reset the context, if it needs it, so that we aren't hanging on to
		 * more than the initial malloc chunk.
		 */
		if (!context->isReset)
			MemoryContextResetOnly(context);

		/*
		 * If the freelist is full, just discard what's already in it.  See
		 * comments with context_freelists[].
		 */
		if (freelist->num_free >= MAX_FREE_CONTEXTS)
		{
			while (freelist->first_free != NULL)
			{
				AllocSetContext *oldset = freelist->first_free;

				freelist->first_free = (AllocSetContext *) oldset->header.nextchild;
				freelist->num_free--;

				/* All that remains is to free the header/initial block */
				free(oldset);
			}
			Assert(freelist->num_free == 0);
		}

		/* Now add the just-deleted context to the freelist. */
		set->header.nextchild = (MemoryContext) freelist->first_free;
		freelist->first_free = set;
		freelist->num_free++;

		return;
	}

	/* Free all blocks, except the keeper which is part of context header */
	while (block != NULL)
	{
		AllocBlock	next = block->next;

		if (block != set->keeper)
			context->mem_allocated -= block->endptr - ((char *) block);

#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(block, block->freeptr - ((char *) block));
#endif

		if (block != set->keeper)
			free(block);

		block = next;
	}

	Assert(context->mem_allocated == keepersize);

	/* Finally, free the context header, including the keeper block */
	free(set);
}

/*
 * AllocSetAlloc
 *		Returns pointer to allocated memory of given size or NULL if
 *		request could not be completed; memory is added to the set.
 *
 * No request may exceed:
 *		MAXALIGN_DOWN(SIZE_MAX) - ALLOC_BLOCKHDRSZ - ALLOC_CHUNKHDRSZ
 * All callers use a much-lower limit.
 *
 * Note: when using valgrind, it doesn't matter how the returned allocation
 * is marked, as mcxt.c will set it to UNDEFINED.  In some paths we will
 * return space that is marked NOACCESS - AllocSetRealloc has to beware!
 */
void *
AllocSetAlloc(MemoryContext context, Size size)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block;
	int			fidx;
	Size		chunk_size;
	Size		blksize;
	void	   *pointer;

	AssertArg(AllocSetIsValid(set));

	/*
	 * If requested size exceeds maximum for chunks, allocate an entire block
	 * for this request.
	 */
	if (size > set->allocChunkLimit)
	{
		Size		genhdrsize;

		chunk_size = MAXALIGN(size);
		/* err on the side of caution and assume it'll be a large chunk */
		genhdrsize = GenericChunkHeaderSize(ALLOC_BLOCKHDRSZ + LARGE_CHUNK_SIZE, chunk_size);
		blksize = ALLOC_BLOCKHDRSZ + genhdrsize + chunk_size;
		block = (AllocBlock) malloc(blksize);
		if (block == NULL)
			return NULL;

		context->mem_allocated += blksize;

		block->aset = set;
		/* mark if this is a large chunk or not */
		block->large_chunk = (genhdrsize == LARGE_CHUNK_SIZE);
		block->freeptr = block->endptr = ((char *) block) + blksize;

		pointer = (void *) (((char *) block) + ALLOC_BLOCKHDRSZ + genhdrsize);

#ifdef MEMORY_CONTEXT_CHECKING
		GenericChunkHeaderEncode(pointer, block, chunk_size, size, MCTX_ASET_ID);

		/* set mark to catch clobber of "unused" space */
		if (size < chunk_size)
			set_sentinel(pointer, size);
#else
		GenericChunkHeaderEncode(pointer, block, chunk_size, MCTX_ASET_ID);
#endif

#ifdef RANDOMIZE_ALLOCATED_MEMORY
		/* fill the allocated space with junk */
		randomize_mem((char *) pointer, size);
#endif

		/*
		 * Stick the new block underneath the active allocation block, if any,
		 * so that we don't lose the use of the space remaining therein.
		 */
		if (set->blocks != NULL)
		{
			block->prev = set->blocks;
			block->next = set->blocks->next;
			if (block->next)
				block->next->prev = block;
			set->blocks->next = block;
		}
		else
		{
			block->prev = NULL;
			block->next = NULL;
			set->blocks = block;
		}

		/* Ensure any padding bytes are marked NOACCESS. */
		VALGRIND_MAKE_MEM_NOACCESS(((char *) pointer) + size,
								   chunk_size - size);

		Assert(GetMemoryChunkMethodID(pointer) == MCTX_ASET_ID);

		return pointer;
	}

	/*
	 * Request is small enough to be treated as a chunk.  Look in the
	 * corresponding free list to see if there is a free chunk we could reuse.
	 * If one is found, remove it from the free list, make it again a member
	 * of the alloc set and return its data address.
	 */
	fidx = AllocSetFreeIndex(size);

	/*
	 * Choose the actual chunk size to allocate.
	 */
	chunk_size = (1 << ALLOC_MINBITS) << fidx;
	Assert(chunk_size >= size);

	pointer = set->freelist[fidx];
	if (pointer != NULL)
	{
		/*
		 * Point the freelist head at the next free chunk which we stored in
		 * the allocated memory
		 */
		set->freelist[fidx] = *((void **) pointer);

#ifdef MEMORY_CONTEXT_CHECKING
		{
			/*
			 * The chunk header is already set correctly when not doing
			 * MEMORY_CONTEXT_CHECKING. Here we just need to update the
			 * requested size.  We must decode it to get the block offset
			 * before we reencode it again.
			 */
			AllocBlock block;
			Size chksz, blkoff, reqsz;

			GenericChunkHeaderDecode(pointer, &chksz, &blkoff, &reqsz);

			block = (AllocBlock) ((char *) pointer - blkoff);

			GenericChunkHeaderEncode(pointer, block, chunk_size, size, MCTX_ASET_ID);
		}

		/* set mark to catch clobber of "unused" space */
		if (size < chunk_size)
			set_sentinel(pointer, size);
#endif

#ifdef RANDOMIZE_ALLOCATED_MEMORY
		/* fill the allocated space with junk */
		randomize_mem((char *) pointer, size);
#endif

		/* Ensure any padding bytes are marked NOACCESS. */
		VALGRIND_MAKE_MEM_NOACCESS(((char *) pointer) + size,
								   chunk_size - size);

		Assert(GetMemoryChunkMethodID(pointer) == MCTX_ASET_ID);

		return pointer;
	}

	/*
	 * If there is enough room in the active allocation block, we will put the
	 * chunk into that block.  Else must start a new one.
	 */
	if ((block = set->blocks) != NULL)
	{
		Size		availspace = block->endptr - block->freeptr;

		Assert(availspace <= SMALL_CHUNK_LIMIT);

		if (availspace < (SMALL_CHUNK_SIZE + chunk_size))
		{
			/*
			 * The existing active (top) block does not have enough room for
			 * the requested allocation, but it might still have a useful
			 * amount of space in it.  Once we push it down in the block list,
			 * we'll never try to allocate more space from it. So, before we
			 * do that, carve up its free space into chunks that we can put on
			 * the set's freelists.
			 *
			 * Because we can only get here when there's less than
			 * ALLOC_CHUNK_LIMIT left in the block, this loop cannot iterate
			 * more than ALLOCSET_NUM_FREELISTS-1 times.
			 */
			while (availspace >= ((1 << ALLOC_MINBITS) + SMALL_CHUNK_SIZE + sizeof(void *)))
			{
				Size		availchunk = availspace - SMALL_CHUNK_SIZE;
				int			a_fidx = AllocSetFreeIndex(availchunk);

				/*
				 * In most cases, we'll get back the index of the next larger
				 * freelist than the one we need to put this chunk on.  The
				 * exception is when availchunk is exactly a power of 2.
				 */
				if (availchunk != ((Size) 1 << (a_fidx + ALLOC_MINBITS)))
				{
					a_fidx--;
					Assert(a_fidx >= 0);
					availchunk = ((Size) 1 << (a_fidx + ALLOC_MINBITS));
				}

				pointer = (void *) (block->freeptr + SMALL_CHUNK_SIZE);

				block->freeptr += (availchunk + SMALL_CHUNK_SIZE);
				availspace -= (availchunk + SMALL_CHUNK_SIZE);

#ifdef MEMORY_CONTEXT_CHECKING
				/* mark it free */
				GenericChunkHeaderEncode(pointer, block, availchunk, 0, MCTX_ASET_ID);
#else
				GenericChunkHeaderEncode(pointer, block, availchunk, MCTX_ASET_ID);
#endif
				/* push this chunk onto the freelist */
				*((void **) pointer) = set->freelist[a_fidx];
				set->freelist[a_fidx] = pointer;
			}
			/* Mark that we need to create a new block */
			block = NULL;
		}
	}

	/*
	 * Time to create a new regular (multi-chunk) block?
	 */
	if (block == NULL)
	{
		Size		required_size;

		/*
		 * The first such block has size initBlockSize, and we double the
		 * space in each succeeding block, but not more than maxBlockSize.
		 */
		blksize = set->nextBlockSize;
		set->nextBlockSize <<= 1;
		if (set->nextBlockSize > set->maxBlockSize)
			set->nextBlockSize = set->maxBlockSize;

		/*
		 * If initBlockSize is less than ALLOC_CHUNK_LIMIT, we could need more
		 * space... but try to keep it a power of 2.
		 */
		required_size = ALLOC_BLOCKHDRSZ + SMALL_CHUNK_SIZE + chunk_size;
		while (blksize < required_size)
			blksize <<= 1;

		/* Try to allocate it */
		block = (AllocBlock) malloc(blksize);

		/*
		 * We could be asking for pretty big blocks here, so cope if malloc
		 * fails.  But give up if there's less than 1 MB or so available...
		 */
		while (block == NULL && blksize > 1024 * 1024)
		{
			blksize >>= 1;
			if (blksize < required_size)
				break;
			block = (AllocBlock) malloc(blksize);
		}

		if (block == NULL)
			return NULL;

		context->mem_allocated += blksize;

		block->aset = set;
		block->large_chunk = false;
		block->freeptr = ((char *) block) + ALLOC_BLOCKHDRSZ;
		block->endptr = ((char *) block) + blksize;

		/* Mark unallocated space NOACCESS. */
		VALGRIND_MAKE_MEM_NOACCESS(block->freeptr,
								   blksize - ALLOC_BLOCKHDRSZ);

		block->prev = NULL;
		block->next = set->blocks;
		if (block->next)
			block->next->prev = block;
		set->blocks = block;
	}

	/*
	 * OK, do the allocation
	 */
	pointer = (void *) (block->freeptr + SMALL_CHUNK_SIZE);

	block->freeptr += (SMALL_CHUNK_SIZE + chunk_size);
	Assert(block->freeptr <= block->endptr);


#ifdef MEMORY_CONTEXT_CHECKING
	GenericChunkHeaderEncode(pointer, block, chunk_size, size, MCTX_ASET_ID);

	/* set mark to catch clobber of "unused" space */
	if (size < chunk_size)
		set_sentinel(pointer, size);
#else
	GenericChunkHeaderEncode(pointer, block, chunk_size, MCTX_ASET_ID);
#endif

#ifdef RANDOMIZE_ALLOCATED_MEMORY
	/* fill the allocated space with junk */
	randomize_mem((char *) pointer, size);
#endif

	/* Ensure any padding bytes are marked NOACCESS. */
	VALGRIND_MAKE_MEM_NOACCESS((char *) pointer + size,
							   chunk_size - size);

	Assert(GetMemoryChunkMethodID(pointer) == MCTX_ASET_ID);

	return pointer;
}

/*
 * AllocSetFree
 *		Frees allocated memory; memory is removed from the set.
 */
void
AllocSetFree(void *pointer)
{
	AllocBlock	block;
	AllocSet	set;
	Size		chunksize;
	Size		blockoffset;
	Size		hdrsize;
#ifdef MEMORY_CONTEXT_CHECKING
	Size		reqsize;

	hdrsize = GenericChunkHeaderDecode(pointer, &chunksize, &blockoffset, &reqsize);
#else
	hdrsize = GenericChunkHeaderDecode(pointer, &chunksize, &blockoffset);
#endif

	block = (AllocBlock) ((char *) pointer - blockoffset);
	set = block->aset;

#ifdef MEMORY_CONTEXT_CHECKING
	/* Test for someone scribbling on unused space in chunk */
	if (reqsize < chunksize)
		if (!sentinel_ok(pointer, reqsize))
			elog(WARNING, "detected write past chunk end in %s %p",
				 set->header.name, pointer);
#endif

	if (chunksize > set->allocChunkLimit)
	{
		/*
		 * Big chunks are certain to have been allocated as single-chunk
		 * blocks.  Just unlink that block and return it to malloc().
		 */

		/*
		 * Try to verify that we have a sane block pointer: it should
		 * reference the correct aset, and freeptr and endptr should point
		 * just past the chunk.
		 */
		if (block->freeptr != block->endptr ||
			block->freeptr != ((char *) block) +
			(ALLOC_BLOCKHDRSZ + hdrsize + chunksize))
			elog(ERROR, "could not find block containing pointer %p", pointer);

		/* OK, remove block from aset's list and free it */
		if (block->prev)
			block->prev->next = block->next;
		else
			set->blocks = block->next;
		if (block->next)
			block->next->prev = block->prev;

		set->header.mem_allocated -= block->endptr - ((char *) block);

#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(block, block->freeptr - ((char *) block));
#endif
		free(block);
	}
	else
	{
		/* Normal case, put the chunk into appropriate freelist */
		int			fidx = AllocSetFreeIndex(chunksize);

		/* Use the pointer to store the freelist link */
		*((void **) pointer) = set->freelist[fidx];

#ifdef CLOBBER_FREED_MEMORY
		/* Wipe the memory, but not the freelist link we just set above */
		wipe_mem(((char *) pointer) + sizeof(void *), chunksize - sizeof(void *));
#endif

#ifdef MEMORY_CONTEXT_CHECKING
		/* Reset requested_size to 0 in chunks that are on freelist */
		GenericChunkHeaderEncode(pointer, block, chunksize, 0, MCTX_ASET_ID);
#endif

		set->freelist[fidx] = pointer;
	}
}

/*
 * AllocSetRealloc
 *		Returns new pointer to allocated memory of given size or NULL if
 *		request could not be completed; this memory is added to the set.
 *		Memory associated with given pointer is copied into the new memory,
 *		and the old memory is freed.
 *
 * Without MEMORY_CONTEXT_CHECKING, we don't know the old request size.  This
 * makes our Valgrind client requests less-precise, hazarding false negatives.
 * (In principle, we could use VALGRIND_GET_VBITS() to rediscover the old
 * request size.)
 */
void *
AllocSetRealloc(void *pointer, Size size)
{
	AllocBlock	block;
	AllocSet	set;
	Size		oldsize;
	Size		blockoffset;
	Size		hdrsize;
#ifdef MEMORY_CONTEXT_CHECKING
	Size		oldreqsize;

	hdrsize = GenericChunkHeaderDecode(pointer, &oldsize, &blockoffset, &oldreqsize);
#else

	hdrsize = GenericChunkHeaderDecode(pointer, &oldsize, &blockoffset);
#endif

	block = (AllocBlock) ((char *) pointer - blockoffset);
	set = block->aset;

#ifdef MEMORY_CONTEXT_CHECKING
	/* Test for someone scribbling on unused space in chunk */
	if (oldreqsize < oldsize)
		if (!sentinel_ok(pointer, oldreqsize))
			elog(WARNING, "detected write past chunk end in %s %p",
				 set->header.name, pointer);
#endif

	if (oldsize > set->allocChunkLimit)
	{
		/*
		 * The chunk must have been allocated as a single-chunk block.  Use
		 * realloc() to make the containing block bigger, or smaller, with
		 * minimum space wastage.
		 */
		Size		chksize;
		Size		blksize;
		Size		oldblksize;
		Size		newhdrsize;

		/*
		 * Try to verify that we have a sane block pointer: it should
		 * reference the correct aset, and freeptr and endptr should point
		 * just past the chunk.
		 */
		if (block->freeptr != block->endptr ||
			block->freeptr != ((char *) block) +
			(ALLOC_BLOCKHDRSZ + hdrsize + oldsize))
			elog(ERROR, "could not find block containing pointer %p", pointer);

		/*
		 * Even if the new request is less than set->allocChunkLimit, we stick
		 * with the single-chunk block approach.  Therefore we need
		 * chunk->size to be bigger than set->allocChunkLimit, so we don't get
		 * confused about the chunk's status in future calls.
		 */
		chksize = Max(size, set->allocChunkLimit + 1);
		chksize = MAXALIGN(chksize);

		/* Assume big chunk size */
		newhdrsize = GenericChunkHeaderSize(ALLOC_BLOCKHDRSZ + LARGE_CHUNK_SIZE, chksize);

		/* Do the realloc */
		blksize = ALLOC_BLOCKHDRSZ + newhdrsize + chksize;
		oldblksize = block->endptr - ((char *) block);

		if (newhdrsize == hdrsize)
			block = (AllocBlock) realloc(block, blksize);
		else
		{
			/*
			 * When the size of the chunk header changes, we're unable to
			 * simply call realloc as the offset between the block and the
			 * pointer will have changed.  For this case we resort to just
			 * allocating new memory with AllocSetAlloc then copying the old
			 * memory over before finally freeing the old memory and block
			 * with AllocSetFree.
			 */
			void *newPointer = AllocSetAlloc((MemoryContext) set, size);

			if (newPointer == NULL)
				return NULL;

			memcpy(newPointer, pointer, Min(oldsize, size));

			AllocSetFree(pointer);

			return newPointer;
		}

		if (block == NULL)
			return NULL;

		/* updated separately, not to underflow when (oldblksize > blksize) */
		set->header.mem_allocated -= oldblksize;
		set->header.mem_allocated += blksize;

		block->freeptr = block->endptr = ((char *) block) + blksize;

		/* Update pointers since block has likely been moved */
		pointer = (void *) ((char *) block + ALLOC_BLOCKHDRSZ + newhdrsize);
		if (block->prev)
			block->prev->next = block;
		else
			set->blocks = block;
		if (block->next)
			block->next->prev = block;

#ifdef MEMORY_CONTEXT_CHECKING
		GenericChunkHeaderEncode(pointer, block, chksize, size, MCTX_ASET_ID);
#else
		GenericChunkHeaderEncode(pointer, block, chksize, MCTX_ASET_ID);
#endif

#ifdef MEMORY_CONTEXT_CHECKING
#ifdef RANDOMIZE_ALLOCATED_MEMORY
		/* for enlarged chunks, randomize the enlarged portion. */
		if (size > oldreqsize)
			randomize_mem(((char *) pointer) + oldreqsize, size - oldreqsize);
#endif

		/*
		 * realloc() (or randomize_mem()) will have left any newly-allocated
		 * part UNDEFINED, but we may need to adjust trailing bytes from the
		 * old allocation.
		 */
#ifdef USE_VALGRIND
		if (oldsize > oldreqsize)
			VALGRIND_MAKE_MEM_UNDEFINED((char *) pointer + oldreqsize,
										oldsize - oldreqsize);
#endif
		/* set mark to catch clobber of "unused" space */
		if (size < chksize)
			set_sentinel(pointer, size);
#else							/* !MEMORY_CONTEXT_CHECKING */

		/*
		 * We don't know how much of the old chunk size was the actual
		 * allocation; it could have been as small as one byte.  We have to be
		 * conservative and just mark the entire old portion DEFINED.
		 */
		VALGRIND_MAKE_MEM_DEFINED(pointer, oldsize);
#endif

		/* Ensure any padding bytes are marked NOACCESS. */
		VALGRIND_MAKE_MEM_NOACCESS((char *) pointer + size, chksize - size);

		Assert(GetMemoryChunkMethodID(pointer) == MCTX_ASET_ID);

		return pointer;
	}

	/*
	 * Chunk sizes are aligned to power of 2 in AllocSetAlloc().  Maybe the
	 * allocated area already is >= the new size.  (In particular, we will
	 * fall out here if the requested size is a decrease.)
	 */
	else if (oldsize >= size)
	{
#ifdef MEMORY_CONTEXT_CHECKING
#ifdef RANDOMIZE_ALLOCATED_MEMORY
		/* We can only fill the extra space if we know the prior request */
		if (size > oldreqsize)
			randomize_mem((char *) pointer + oldreqsize,
						  size - oldreqsize);
#endif
		/*
		 * If this is an increase, mark any newly-available part UNDEFINED.
		 * Otherwise, mark the obsolete part NOACCESS.
		 */
		if (size > oldreqsize)
			VALGRIND_MAKE_MEM_UNDEFINED((char *) pointer + oldreqsize,
										size - oldreqsize);
		else
			VALGRIND_MAKE_MEM_NOACCESS((char *) pointer + size,
									   oldsize - size);

		/* set mark to catch clobber of "unused" space */
		if (size < oldsize)
			set_sentinel(pointer, size);

		/* Re-encode the chunk to apply new request size */
		GenericChunkHeaderEncode(pointer, block, oldsize, size, MCTX_ASET_ID);
#else							/* !MEMORY_CONTEXT_CHECKING */

		/*
		 * We don't have the information to determine whether we're growing
		 * the old request or shrinking it, so we conservatively mark the
		 * entire new allocation DEFINED.
		 */
		VALGRIND_MAKE_MEM_NOACCESS(pointer, oldsize);
		VALGRIND_MAKE_MEM_DEFINED(pointer, size);
#endif

		Assert(GetMemoryChunkMethodID(pointer) == MCTX_ASET_ID);

		return pointer;
	}
	else
	{
		/*
		 * Enlarge-a-small-chunk case.  We just do this by brute force, ie,
		 * allocate a new chunk and copy the data.  Since we know the existing
		 * data isn't huge, this won't involve any great memcpy expense, so
		 * it's not worth being smarter.  (At one time we tried to avoid
		 * memcpy when it was possible to enlarge the chunk in-place, but that
		 * turns out to misbehave unpleasantly for repeated cycles of
		 * palloc/repalloc/pfree: the eventually freed chunks go into the
		 * wrong freelist for the next initial palloc request, and so we leak
		 * memory indefinitely.  See pgsql-hackers archives for 2007-08-11.)
		 */
		AllocPointer newPointer;

		/* allocate new chunk */
		newPointer = AllocSetAlloc((MemoryContext) set, size);

		/* leave immediately if request was not completed */
		if (newPointer == NULL)
			return NULL;

		/*
		 * AllocSetAlloc() may have returned a region that is still NOACCESS.
		 * Change it to UNDEFINED for the moment; memcpy() will then transfer
		 * definedness from the old allocation to the new.  If we know the old
		 * allocation, copy just that much.  Otherwise, make the entire old
		 * chunk defined to avoid errors as we copy the currently-NOACCESS
		 * trailing bytes.
		 */
		VALGRIND_MAKE_MEM_UNDEFINED(newPointer, size);

#ifdef MEMORY_CONTEXT_CHECKING
		oldsize = oldreqsize;
#else
		VALGRIND_MAKE_MEM_DEFINED(pointer, oldsize);
#endif

		/* transfer existing data (certain to fit) */
		memcpy(newPointer, pointer, oldsize);

		/* free old chunk */
		AllocSetFree(pointer);

		Assert(GetMemoryChunkMethodID(newPointer) == MCTX_ASET_ID);

		return newPointer;
	}
}

/*
 * AllocSetGetChunkContext
 *		Return the MemoryContext that 'pointer' belongs to.
 */
MemoryContext
AllocSetGetChunkContext(void *pointer)
{
	AllocBlock	block;
	AllocSet	set;
	Size		chunksize;
	Size		blockoffset;
#ifdef MEMORY_CONTEXT_CHECKING
	Size		reqsize;

	GenericChunkHeaderDecode(pointer, &chunksize, &blockoffset, &reqsize);
#else
	GenericChunkHeaderDecode(pointer, &chunksize, &blockoffset);
#endif

	block = (AllocBlock) ((char *) pointer - blockoffset);

	set = block->aset;

	return &set->header;
}

/*
 * AllocSetGetChunkSpace
 *		Given a currently-allocated chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 */
Size
AllocSetGetChunkSpace(void *pointer)
{
	Size		chunksize;
	Size		blockoffset;
	Size		hdrsize;
#ifdef MEMORY_CONTEXT_CHECKING
	Size		reqsize;

	hdrsize = GenericChunkHeaderDecode(pointer, &chunksize, &blockoffset, &reqsize);
#else
	hdrsize = GenericChunkHeaderDecode(pointer, &chunksize, &blockoffset);
#endif

	return hdrsize + chunksize;
}

/*
 * AllocSetIsEmpty
 *		Is an allocset empty of any allocated space?
 */
bool
AllocSetIsEmpty(MemoryContext context)
{
	/*
	 * For now, we say "empty" only if the context is new or just reset. We
	 * could examine the freelists to determine if all space has been freed,
	 * but it's not really worth the trouble for present uses of this
	 * functionality.
	 */
	if (context->isReset)
		return true;
	return false;
}

/*
 * AllocSetStats
 *		Compute stats about memory consumption of an allocset.
 *
 * printfunc: if not NULL, pass a human-readable stats string to this.
 * passthru: pass this pointer through to printfunc.
 * totals: if not NULL, add stats about this context into *totals.
 * print_to_stderr: print stats to stderr if true, elog otherwise.
 */
void
AllocSetStats(MemoryContext context,
			  MemoryStatsPrintFunc printfunc, void *passthru,
			  MemoryContextCounters *totals, bool print_to_stderr)
{
	AllocSet	set = (AllocSet) context;
	Size		nblocks = 0;
	Size		freechunks = 0;
	Size		totalspace;
	Size		freespace = 0;
	AllocBlock	block;
	int			fidx;

	/* Include context header in totalspace */
	totalspace = MAXALIGN(sizeof(AllocSetContext));

	for (block = set->blocks; block != NULL; block = block->next)
	{
		nblocks++;
		totalspace += block->endptr - ((char *) block);
		freespace += block->endptr - block->freeptr;
	}
	for (fidx = 0; fidx < ALLOCSET_NUM_FREELISTS; fidx++)
	{
		void	   *pointer = set->freelist[fidx];
		Size		chunk_size = (1 << ALLOC_MINBITS) << fidx;

		while (pointer != NULL)
		{
			freechunks++;
			freespace += chunk_size + SMALL_CHUNK_SIZE;

			/*
			 * Set pointer to the next freelist item which is stored in the
			 * pointed to memory.
			 */
			pointer = *((void **) pointer);
		}
	}

	if (printfunc)
	{
		char		stats_string[200];

		snprintf(stats_string, sizeof(stats_string),
				 "%zu total in %zu blocks; %zu free (%zu chunks); %zu used",
				 totalspace, nblocks, freespace, freechunks,
				 totalspace - freespace);
		printfunc(context, passthru, stats_string, print_to_stderr);
	}

	if (totals)
	{
		totals->nblocks += nblocks;
		totals->freechunks += freechunks;
		totals->totalspace += totalspace;
		totals->freespace += freespace;
	}
}


#ifdef MEMORY_CONTEXT_CHECKING

/*
 * AllocSetCheck
 *		Walk through chunks and check consistency of memory.
 *
 * NOTE: report errors as WARNING, *not* ERROR or FATAL.  Otherwise you'll
 * find yourself in an infinite loop when trouble occurs, because this
 * routine will be entered again when elog cleanup tries to release memory!
 */
void
AllocSetCheck(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	const char *name = set->header.name;
	AllocBlock	prevblock;
	AllocBlock	block;
	Size		total_allocated = 0;

	for (prevblock = NULL, block = set->blocks;
		 block != NULL;
		 prevblock = block, block = block->next)
	{
		char	   *bpoz = ((char *) block) + ALLOC_BLOCKHDRSZ;
		long		blk_used = block->freeptr - bpoz;
		long		blk_data = 0;
		long		nchunks = 0;
		Size		expected_hdrsize;

		if (set->keeper == block)
			total_allocated += block->endptr - ((char *) set);
		else
			total_allocated += block->endptr - ((char *) block);

		/*
		 * Empty block - empty can be keeper-block only
		 */
		if (!blk_used)
		{
			if (set->keeper != block)
				elog(WARNING, "problem in alloc set %s: empty block %p",
					 name, block);
		}

		/*
		 * Check block header fields
		 */
		if (block->aset != set ||
			block->prev != prevblock ||
			block->freeptr < bpoz ||
			block->freeptr > block->endptr)
			elog(WARNING, "problem in alloc set %s: corrupt header in block %p",
				 name, block);

		if (block->large_chunk)
			expected_hdrsize = LARGE_CHUNK_SIZE;
		else
			expected_hdrsize = SMALL_CHUNK_SIZE;

		/*
		 * Chunk walker
		 */
		while (bpoz < block->freeptr)
		{
			void	   *pointer;
			Size		chsize;
			Size		blkoff;
			Size		hdrsize;
			Size		reqsize;

			pointer = bpoz + expected_hdrsize;
			hdrsize = GenericChunkHeaderDecode(pointer, &chsize, &blkoff, &reqsize);

			/*
			 * Check chunk size
			 */
			if (expected_hdrsize != hdrsize)
				elog(WARNING, "problem in alloc set %s: expected chunk header size is %zu but actual size is %zu",
					 name, expected_hdrsize, hdrsize);
			if (reqsize > chsize)
				elog(WARNING, "problem in alloc set %s: req size > alloc size for pointer %p in block %p",
					 name, pointer, block);
			if (chsize < (1 << ALLOC_MINBITS))
				elog(WARNING, "problem in alloc set %s: bad size %zu for pointer %p in block %p",
					 name, chsize, pointer, block);

			/* single-chunk block? */
			if (chsize > set->allocChunkLimit &&
				chsize + expected_hdrsize != blk_used)
				elog(WARNING, "problem in alloc set %s: bad single-chunk pointer %p in block %p",
					 name, pointer, block);

			/* Check the block offset correctly matches this block */
			if ((AllocBlock) ((char *) pointer - blkoff) != block)
				elog(WARNING, "problem in alloc set %s: bad block offset for pointer %p in block %p",
					 name, pointer, block);

			/*
			 * Check for overwrite of padding space in an allocated chunk.
			 */
			if (reqsize > 0 && reqsize < chsize &&
				!sentinel_ok(pointer, reqsize))
				elog(WARNING, "problem in alloc set %s: detected write past chunk end in block %p, pointer %p",
					 name, block, pointer);

			blk_data += chsize;
			nchunks++;

			bpoz += expected_hdrsize + chsize;
		}

		if (bpoz != block->freeptr)
			elog(WARNING, "problem in alloc set %s: found inconsistent memory block %p",
				 name, block);
	}

	Assert(total_allocated == context->mem_allocated);
}

#endif							/* MEMORY_CONTEXT_CHECKING */
