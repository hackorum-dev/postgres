/*-------------------------------------------------------------------------
 *
 * pg_compact_test.c
 *	  Exercise heap-compaction primitives in isolation.
 *
 *	  Currently exposes pg_test_compact_buffer(), a thin wrapper around
 *	  RelationGetSpecificBufferForTuple() so the page-targeting logic can
 *	  be smoke-tested before VACUUM (COMPACT) is built on top of it.
 *
 *	  contrib/pg_compact_test/pg_compact_test.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/hio.h"
#include "access/htup_details.h"
#include "catalog/pg_am.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_compact_test",
					.version = PG_VERSION
);

PG_FUNCTION_INFO_V1(pg_test_compact_buffer);
PG_FUNCTION_INFO_V1(pg_test_relocate_tuple);

/*
 * pg_test_compact_buffer(rel regclass,
 *                        target_block bigint,
 *                        source_block bigint,
 *                        tuple_size integer) RETURNS boolean
 *
 *	Pins source_block (without taking its content lock), then calls
 *	RelationGetSpecificBufferForTuple() asking for tuple_size bytes of
 *	free space on target_block, with source_block as the "other" buffer.
 *
 *	Returns true if the call succeeded (the target page had room and is
 *	now exclusively locked); the locks and pins are released before
 *	returning.  Returns false if the target page did not have room (the
 *	function returned InvalidBuffer).
 *
 *	This is intended for testing only.  It does not actually move any
 *	tuple -- the goal is to exercise the buffer-targeting and locking
 *	logic.  The relation is opened with RowExclusiveLock to match the
 *	lock level VACUUM (COMPACT) will eventually use when reusing this
 *	primitive.
 */
Datum
pg_test_compact_buffer(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		target_block_arg = PG_GETARG_INT64(1);
	int64		source_block_arg = PG_GETARG_INT64(2);
	int32		tuple_size = PG_GETARG_INT32(3);
	Relation	rel;
	BlockNumber nblocks;
	BlockNumber target_block;
	BlockNumber source_block;
	Buffer		source_buffer;
	Buffer		target_buffer;
	Buffer		vmbuffer = InvalidBuffer;
	Buffer		vmbuffer_other = InvalidBuffer;

	if (target_block_arg < 0 || target_block_arg > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("target_block out of range: %lld",
						(long long) target_block_arg)));
	if (source_block_arg < 0 || source_block_arg > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("source_block out of range: %lld",
						(long long) source_block_arg)));
	if (tuple_size <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("tuple_size must be positive")));

	target_block = (BlockNumber) target_block_arg;
	source_block = (BlockNumber) source_block_arg;

	if (target_block == source_block)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("target_block must differ from source_block")));

	rel = relation_open(relid, RowExclusiveLock);

	if (!RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is of wrong relation kind",
						RelationGetRelationName(rel))));

	if (rel->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_test_compact_buffer requires the heap table AM")));

	nblocks = RelationGetNumberOfBlocks(rel);
	if (target_block >= nblocks || source_block >= nblocks)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("block number out of range for relation \"%s\" (size %u)",
						RelationGetRelationName(rel), nblocks)));

	/*
	 * Pin the source buffer.  RelationGetSpecificBufferForTuple expects an
	 * unlocked buffer here and arranges the buffer-lock acquisition order
	 * itself.
	 */
	source_buffer = ReadBuffer(rel, source_block);

	target_buffer = RelationGetSpecificBufferForTuple(rel,
													  (Size) tuple_size,
													  target_block,
													  source_buffer,
													  &vmbuffer,
													  &vmbuffer_other);

	if (target_buffer == InvalidBuffer)
	{
		/*
		 * The function already released the lock and pin on target_buffer.
		 * It only released the *lock* on source_buffer (it never held one
		 * to begin with on entry, and on the no-space path it explicitly
		 * unlocks otherBuffer).  We still hold its pin.
		 */
		ReleaseBuffer(source_buffer);
		if (vmbuffer != InvalidBuffer)
			ReleaseBuffer(vmbuffer);
		if (vmbuffer_other != InvalidBuffer)
			ReleaseBuffer(vmbuffer_other);
		relation_close(rel, RowExclusiveLock);
		PG_RETURN_BOOL(false);
	}

	/*
	 * Success: both buffers are exclusive-locked and pinned.  Release
	 * everything without modifying the pages.
	 */
	UnlockReleaseBuffer(target_buffer);
	LockBuffer(source_buffer, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(source_buffer);
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);
	if (vmbuffer_other != InvalidBuffer)
		ReleaseBuffer(vmbuffer_other);

	relation_close(rel, RowExclusiveLock);
	PG_RETURN_BOOL(true);
}

/*
 * pg_test_relocate_tuple(rel regclass, source_tid tid, target_block bigint)
 *     RETURNS tid
 *
 *	Smoke-test wrapper for heap_relocate.  Returns the new TID of the
 *	relocated tuple, or NULL if heap_relocate refused (concurrent activity,
 *	target page lost its room, etc.).
 *
 *	Note: this wrapper deliberately does NOT update indexes.  The post-
 *	relocation index-fixup pass requires full executor scaffolding
 *	(snapshot, ranges, ECxt) that a contrib function cannot reasonably
 *	construct by hand; that responsibility belongs to the VACUUM (COMPACT)
 *	orchestrator, which has the right context.  Until that lands, callers
 *	can verify the relocation via TID-based queries (WHERE ctid = ...) and
 *	sequential scans, but index-based lookups on the relocated row will
 *	miss it.
 */
Datum
pg_test_relocate_tuple(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ItemPointer source_tid = (ItemPointer) PG_GETARG_POINTER(1);
	int64		target_block_arg = PG_GETARG_INT64(2);
	BlockNumber target_block;
	BlockNumber nblocks;
	Relation	rel;
	TM_Result	result;
	TM_FailureData tmfd;
	TU_UpdateIndexes update_indexes;
	ItemPointerData new_tid;
	ItemPointer ret_tid;

	if (target_block_arg < 0 || target_block_arg > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("target_block out of range: %lld",
						(long long) target_block_arg)));

	target_block = (BlockNumber) target_block_arg;

	rel = relation_open(relid, RowExclusiveLock);

	if (!RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is of wrong relation kind",
						RelationGetRelationName(rel))));

	if (rel->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_test_relocate_tuple requires the heap table AM")));

	nblocks = RelationGetNumberOfBlocks(rel);
	if (target_block >= nblocks ||
		ItemPointerGetBlockNumber(source_tid) >= nblocks)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("block number out of range for relation \"%s\" (size %u)",
						RelationGetRelationName(rel), nblocks)));

	result = heap_relocate(rel, source_tid, target_block,
						   GetCurrentCommandId(true),
						   &tmfd, &update_indexes, &new_tid);

	if (result != TM_Ok)
	{
		relation_close(rel, RowExclusiveLock);
		PG_RETURN_NULL();
	}

	relation_close(rel, RowExclusiveLock);

	ret_tid = (ItemPointer) palloc(sizeof(ItemPointerData));
	ItemPointerCopy(&new_tid, ret_tid);
	PG_RETURN_POINTER(ret_tid);
}
