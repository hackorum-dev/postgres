/*-------------------------------------------------------------------------
*
 * proxy.c
 *	  Proxy allocator definitions.
 *
 * Proxy is a MemoryContext implementation designed for memory usages which
 * require their own memory context, but which generally have few allocations
 * that generally have a very long lifetime.  Compared to ASet, every
 * allocation of a Proxy memory context gets an External chunk.
 *
 * Portions Copyright (c) 2024-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/proxy.c
 *
 *
 *	Proxy is best suited to cases which require a small number of small
 *	long-lived allocations, where the size overhead of a full aset context
 *	are significant.
 *
 *	Allocations are MAXALIGNed.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "lib/ilist.h"
#include "port/pg_bitutils.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/memutils_memorychunk.h"
#include "utils/memutils_internal.h"

typedef struct ProxyContext
{
	MemoryContextData header;	/* Standard memory-context fields */

	dlist_head	allocations;
#ifdef MEMORY_CONTEXT_CHECKING
	int			chunks_allocated;
#endif
} ProxyContext;

typedef struct ProxyChunk
{
	dlist_node	node;
	size_t		sz;
	ProxyContext *context;
} ProxyChunk;


#define ExternalChunkGetBlock(chunk) \
	(ProxyChunk *) ((char *) chunk - MAXALIGN(sizeof(ProxyChunk)))


/*
 * ProxyContextCreate
 *		Create a Proxy memory context
 */
MemoryContext
ProxyContextCreate(MemoryContext parent, const char *name)
{
	Size		allocSize = sizeof(ProxyContext);
	ProxyContext *ctx;

	/*
	 * Allocate the initial block.  Unlike other proxy.c blocks, it starts
	 * with the context header and its block header follows that.
	 */
	ctx = (ProxyContext *) malloc(allocSize);
	if (ctx == NULL)
	{
		MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while creating memory context \"%s\".",
						   name)));
	}

	VALGRIND_CREATE_MEMPOOL(ctx, 0, false);
	VALGRIND_MEMPOOL_ALLOC(ctx, ctx, allocSize);


	dlist_init(&ctx->allocations);
#ifdef MEMORY_CONTEXT_CHECKING
	ctx->chunks_allocated = 0;
#endif

	/* Finally, do the type-independent part of context creation */
	MemoryContextCreate((MemoryContext) ctx, T_ProxyContext, MCTX_PROXY_ID,
						parent, name);

	((MemoryContext) ctx)->mem_allocated = allocSize;

	return (MemoryContext) ctx;
}

/*
 * ProxyAlloc
 *		Alloc memory into the Proxy memory context.
 */
void *
ProxyAlloc(MemoryContext context, Size size, int flags)
{
	ProxyContext *ctx = (ProxyContext *) context;
	size_t		sz;
	ProxyChunk *proxy_chunk;
	MemoryChunk *chunk;

	/* validate 'size' is within the limits for the given 'flags' */
	MemoryContextCheckSize(context, size, flags);

	sz = MAXALIGN(sizeof(ProxyChunk)) + sizeof(MemoryChunk) + MAXALIGN(size);

	proxy_chunk = (ProxyChunk *) malloc(sz);
	if (proxy_chunk == NULL)
		return MemoryContextAllocationFailure(context, size, flags);

	VALGRIND_MEMPOOL_ALLOC(ctx, proxy_chunk, size);

	proxy_chunk->sz = sz;
	context->mem_allocated += sz;

	proxy_chunk->context = ctx;

	dlist_push_tail(&ctx->allocations, &proxy_chunk->node);

	chunk = (MemoryChunk *) (((char *) proxy_chunk) + MAXALIGN(sizeof(ProxyChunk)));

	MemoryChunkSetHdrMaskExternal(chunk, MCTX_PROXY_ID);
#ifdef MEMORY_CONTEXT_CHECKING
	ctx->chunks_allocated++;
#endif

	return MemoryChunkGetPointer(chunk);
}

/*
 * ProxyAlloc
 *		Free memory from the Proxy memory context.
 */
void
ProxyFree(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	ProxyContext *ctx;
	ProxyChunk *proxy_chunk;

	VALGRIND_MAKE_MEM_DEFINED(chunk, sizeof(MemoryChunk));

	Assert(MemoryChunkIsExternal(chunk));

	proxy_chunk = ExternalChunkGetBlock(chunk);
	VALGRIND_MAKE_MEM_DEFINED(proxy_chunk, sizeof(ProxyChunk));

	ctx = proxy_chunk->context;

	dlist_delete_from(&ctx->allocations, &proxy_chunk->node);

	ctx->header.mem_allocated -= proxy_chunk->sz;

	VALGRIND_MEMPOOL_FREE(ctx, proxy_chunk);
#ifdef MEMORY_CONTEXT_CHECKING
	ctx->chunks_allocated--;
#endif

	free(proxy_chunk);
}

/*
 * ProxyAlloc
 *		Realloc memory in the Proxy memory context.
 */
void *
ProxyRealloc(void *pointer, Size size, int flags)
{
	MemoryChunk *old_chunk = PointerGetMemoryChunk(pointer);
	ProxyChunk *proxy_chunk,
			   *old_proxy_chunk;
	MemoryChunk *chunk;
	ProxyContext *ctx;
	size_t		sz;

	VALGRIND_MAKE_MEM_DEFINED(old_chunk, sizeof(MemoryChunk));

	Assert(MemoryChunkIsExternal(old_chunk));

	sz = MAXALIGN(sizeof(ProxyChunk)) + sizeof(MemoryChunk) + MAXALIGN(size);

	old_proxy_chunk = ExternalChunkGetBlock(old_chunk);

	VALGRIND_MAKE_MEM_DEFINED(old_proxy_chunk, sizeof(ProxyChunk));

	ctx = old_proxy_chunk->context;

	proxy_chunk = malloc(sz);
	if (proxy_chunk == NULL)
		return MemoryContextAllocationFailure((MemoryContext) ctx, size, flags);

	VALGRIND_MEMPOOL_ALLOC(ctx, proxy_chunk, size);

	dlist_delete_from(&ctx->allocations, &old_proxy_chunk->node);
	ctx->header.mem_allocated -= old_proxy_chunk->sz;

	chunk = (MemoryChunk *) (((char *) proxy_chunk) + MAXALIGN(sizeof(ProxyChunk)));

	proxy_chunk->context = ctx;
	proxy_chunk->sz = sz;
	dlist_push_tail(&ctx->allocations, &proxy_chunk->node);
	ctx->header.mem_allocated += sz;

	MemoryChunkSetHdrMaskExternal(chunk, MCTX_PROXY_ID);

	memcpy(chunk, old_chunk, Min(proxy_chunk->sz, old_proxy_chunk->sz));

	VALGRIND_MEMPOOL_FREE(ctx, old_proxy_chunk);
	free(old_proxy_chunk);

	return MemoryChunkGetPointer(chunk);
}

/*
 * ProxyReset
 *		Reset the Proxy memory context.
 */
void
ProxyReset(MemoryContext context)
{
	ProxyContext *ctx = (ProxyContext *) context;

	while (!dlist_is_empty(&ctx->allocations))
	{
		ProxyChunk *proxy_chunk =
			dlist_container(ProxyChunk, node,
							dlist_pop_head_node(&ctx->allocations));

		VALGRIND_MEMPOOL_FREE(ctx, proxy_chunk);
		free(proxy_chunk);
	}
#ifdef MEMORY_CONTEXT_CHECKING
	ctx->chunks_allocated = 0;
	ctx->header.mem_allocated = sizeof(ProxyContext);
#endif
}

/*
 * ProxyAlloc
 *		Delete this Proxy memory context.
 */
void
ProxyDelete(MemoryContext context)
{
	ProxyContext *ctx = (ProxyContext *) context;

	ProxyReset(context);

	VALGRIND_DESTROY_MEMPOOL(context);

	free(ctx);
}

/*
 * ProxyGetChunkContext
 *		Return the MemoryContext that 'pointer' belongs to.
 */
MemoryContext
ProxyGetChunkContext(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	ProxyChunk *proxy_chunk;

	VALGRIND_MAKE_MEM_DEFINED(chunk, sizeof(MemoryChunk));

	Assert(MemoryChunkIsExternal(chunk));
	proxy_chunk = ExternalChunkGetBlock(chunk);

	VALGRIND_MAKE_MEM_DEFINED(proxy_chunk, sizeof(ProxyChunk));

	return (MemoryContext) proxy_chunk->context;
}

/*
 * ProxyGetChunkSpace
*		Given a palloc'd chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 */
Size
ProxyGetChunkSpace(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	ProxyChunk *proxy_chunk;

	VALGRIND_MAKE_MEM_DEFINED(chunk, sizeof(MemoryChunk));

	Assert(MemoryChunkIsExternal(chunk));
	proxy_chunk = ExternalChunkGetBlock(chunk);

	VALGRIND_MAKE_MEM_DEFINED(proxy_chunk, sizeof(ProxyChunk));

	return proxy_chunk->sz;
}

/*
 * ProxyIsEmpty
*		Is the ProxyContext empty of any allocated space?
 */
bool
ProxyIsEmpty(MemoryContext context)
{
	ProxyContext *ctx = (ProxyContext *) context;

	return dlist_is_empty(&ctx->allocations);
}

/*
 * ProxyStats
 *		Compute stats about memory consumption of a Proxy context.
 *
 * printfunc: if not NULL, pass a human-readable stats string to this.
 * passthru: pass this pointer through to printfunc.
 * totals: if not NULL, add stats about this context into *totals.
 * print_to_stderr: print stats to stderr if true, elog otherwise.
 */
void
ProxyStats(MemoryContext context, MemoryStatsPrintFunc printfunc,
		   void *passthru, MemoryContextCounters *totals,
		   bool print_to_stderr)
{
	ProxyContext *ctx = (ProxyContext *) context;
	dlist_iter	iter;
	Size		totalspace;
	Size		nchunks = 0;

	totalspace = MAXALIGN(sizeof(ProxyContext));

	dlist_foreach(iter, &ctx->allocations)
	{
		ProxyChunk *proxy_chunk = dlist_container(ProxyChunk, node, iter.cur);

		nchunks++;
		totalspace += proxy_chunk->sz;
	}


	if (printfunc)
	{
		char		stats_string[200];

		snprintf(stats_string, sizeof(stats_string),
				 "%zu total in %zu chunks;",
				 totalspace, nchunks);
		printfunc(context, passthru, stats_string, print_to_stderr);
	}

	if (totals)
	{
		totals->nblocks += nchunks;
		totals->totalspace += totalspace;
	}
}

#ifdef MEMORY_CONTEXT_CHECKING
/*
 * ProxyCheck
 *		Walk through chunks and check consistency of memory.
 *
 * NOTE: report errors as WARNING, *not* ERROR or FATAL.  Otherwise you'll
 * find yourself in an infinite loop when trouble occurs, because this
 * routine will be entered again when elog cleanup tries to release memory!
 */
void
ProxyCheck(MemoryContext context)
{
	ProxyContext *ctx = (ProxyContext *) context;
	const char *name = context->name;
	dlist_iter	iter;
	Size		total_allocated = sizeof(ProxyContext);
	Size		chunks_allocated = 0;

	/* walk all blocks in this context */
	dlist_foreach(iter, &ctx->allocations)
	{
		ProxyChunk *chunk = dlist_container(ProxyChunk, node, iter.cur);

		total_allocated += chunk->sz;
		chunks_allocated++;
	}

	if (chunks_allocated != ctx->chunks_allocated)
	{
		elog(WARNING, "problem in Proxy %s: number of allocated chunks %d does not match header %d",
			 name, (int) chunks_allocated, ctx->chunks_allocated);
	}

	if (chunks_allocated >= INT_MAX)
	{
		elog(WARNING, "problem in Proxy %s: too many chunks allocated in one context",
			 name);
	}

	if (total_allocated != ctx->header.mem_allocated)
	{
		elog(WARNING, "problem in Proxy %s: amount of memory allocated %d does not match header %d",
			 name, (int) total_allocated, ctx->chunks_allocated);
	}
	Assert(total_allocated == context->mem_allocated);
	Assert(chunks_allocated == ctx->chunks_allocated);
}
#endif
