/*-------------------------------------------------------------------------
 *
 * pg_prewarm.c
 *		  prewarming utilities
 *
 * Copyright (c) 2010-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/pg_prewarm/pg_prewarm.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/relation.h"
#include "catalog/index.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/read_stream.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_prewarm",
					.version = PG_VERSION
);

PG_FUNCTION_INFO_V1(pg_prewarm);

typedef enum
{
	PREWARM_PREFETCH,
	PREWARM_READ,
	PREWARM_BUFFER,
} PrewarmType;

static PGIOAlignedBlock blockbuffer;

/*
 * Block interval for checking conflicting lock waiters. Checking every
 * block is too expensive because LockHasWaitersRelation performs a
 * lock-table probe, but we don't want to check too infrequently either, to avoid
 * long stalls when there are waiters.
 */
#define PREWARM_WAITER_CHECK_INTERVAL		1024

/*
 * Check whether any session is waiting for a lock that conflicts with
 * AccessShareLock on the relation.  If so, release the lock to let the
 * waiter proceed, then try to reacquire it.
 *
 * Throws an error if the relation was dropped or truncated past 'next_block' while
 * the lock was not held.
 */
static Relation
pg_prewarm_check_and_yield(Relation rel, Oid relOid, Oid privOid,
						   ForkNumber forkNumber, int64 next_block,
						   int64 blocks_done, int64 *last_block)
{
	int64		nblocks;

	INJECTION_POINT("pg_prewarm-before-check-and-yield", NULL);

	/* Nothing to do if nobody is waiting for a conflicting lock. */
	if (!LockHasWaitersRelation(rel, AccessShareLock))
		return rel;

	/* Release all locks to let the waiter proceed. */
	relation_close(rel, AccessShareLock);
	if (privOid != relOid)
		UnlockRelationOid(privOid, AccessShareLock);

	/* Reacquire in the correct order: parent table before index. */
	if (privOid != relOid)
		LockRelationOid(privOid, AccessShareLock);
	rel = try_relation_open(relOid, AccessShareLock);
	if (rel == NULL)
	{
		if (privOid != relOid)
			UnlockRelationOid(privOid, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("relation was dropped during pg_prewarm after %" PRId64 " blocks",
						blocks_done)));
	}

	/* Check if the fork still exists and has enough blocks. */
	if (!smgrexists(RelationGetSmgr(rel), forkNumber) ||
		(nblocks = RelationGetNumberOfBlocksInFork(rel, forkNumber)) <= next_block)
	{
		relation_close(rel, AccessShareLock);
		if (privOid != relOid)
			UnlockRelationOid(privOid, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("relation was truncated during pg_prewarm after %" PRId64 " blocks",
						blocks_done)));
	}

	/* Adjust endpoint if the relation was partially truncated. */
	if (*last_block >= nblocks)
		*last_block = nblocks - 1;

	return rel;
}

/*
 * pg_prewarm(regclass, mode text, fork text,
 *			  first_block int8, last_block int8)
 *
 * The first argument is the relation to be prewarmed; the second controls
 * how prewarming is done; legal options are 'prefetch', 'read', and 'buffer'.
 * The third is the name of the relation fork to be prewarmed.  The fourth
 * and fifth arguments specify the first and last block to be prewarmed.
 * If the fourth argument is NULL, it will be taken as 0; if the fifth argument
 * is NULL, it will be taken as the number of blocks in the relation.  The
 * return value is the number of blocks successfully prewarmed.
 */
Datum
pg_prewarm(PG_FUNCTION_ARGS)
{
	Oid			relOid;
	text	   *forkName;
	text	   *type;
	int64		first_block;
	int64		last_block;
	int64		nblocks;
	int64		blocks_done = 0;
	int64		block;
	Relation	rel;
	ForkNumber	forkNumber;
	char	   *forkString;
	char	   *ttype;
	PrewarmType ptype;
	AclResult	aclresult;
	char		relkind;
	Oid			privOid;

	/* Basic sanity checking. */
	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation cannot be null")));
	relOid = PG_GETARG_OID(0);
	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("prewarm type cannot be null")));
	type = PG_GETARG_TEXT_PP(1);
	ttype = text_to_cstring(type);
	if (strcmp(ttype, "prefetch") == 0)
		ptype = PREWARM_PREFETCH;
	else if (strcmp(ttype, "read") == 0)
		ptype = PREWARM_READ;
	else if (strcmp(ttype, "buffer") == 0)
		ptype = PREWARM_BUFFER;
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid prewarm type"),
				 errhint("Valid prewarm types are \"prefetch\", \"read\", and \"buffer\".")));
		PG_RETURN_INT64(0);		/* Placate compiler. */
	}
	if (PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation fork cannot be null")));
	forkName = PG_GETARG_TEXT_PP(2);
	forkString = text_to_cstring(forkName);
	forkNumber = forkname_to_number(forkString);

	/*
	 * Open relation and check privileges.  If the relation is an index, we
	 * must check the privileges on its parent table instead.
	 */
	relkind = get_rel_relkind(relOid);
	if (relkind == RELKIND_INDEX ||
		relkind == RELKIND_PARTITIONED_INDEX)
	{
		privOid = IndexGetRelation(relOid, true);

		/* Lock table before index to avoid deadlock. */
		if (OidIsValid(privOid))
			LockRelationOid(privOid, AccessShareLock);
	}
	else
		privOid = relOid;

	rel = relation_open(relOid, AccessShareLock);

	/*
	 * It's possible that the relation with OID "privOid" was dropped and the
	 * OID was reused before we locked it.  If that happens, we could be left
	 * with the wrong parent table OID, in which case we must ERROR.  It's
	 * possible that such a race would change the outcome of
	 * get_rel_relkind(), too, but the worst case scenario there is that we'll
	 * check privileges on the index instead of its parent table, which isn't
	 * too terrible.
	 */
	if (!OidIsValid(privOid) ||
		(privOid != relOid &&
		 privOid != IndexGetRelation(relOid, true)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("could not find parent table of index \"%s\"",
						RelationGetRelationName(rel))));

	aclresult = pg_class_aclcheck(privOid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind), get_rel_name(relOid));

	/* Check that the relation has storage. */
	if (!RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" does not have storage",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

	/* Check that the fork exists. */
	if (!smgrexists(RelationGetSmgr(rel), forkNumber))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fork \"%s\" does not exist for this relation",
						forkString)));

	/* Validate block numbers, or handle nulls. */
	nblocks = RelationGetNumberOfBlocksInFork(rel, forkNumber);
	if (PG_ARGISNULL(3))
		first_block = 0;
	else
	{
		first_block = PG_GETARG_INT64(3);
		if (first_block < 0 || first_block >= nblocks)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("starting block number must be between 0 and %" PRId64,
							(nblocks - 1))));
	}
	if (PG_ARGISNULL(4))
		last_block = nblocks - 1;
	else
	{
		last_block = PG_GETARG_INT64(4);
		if (last_block < 0 || last_block >= nblocks)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ending block number must be between 0 and %" PRId64,
							(nblocks - 1))));
	}

	/* Now we're ready to do the real work. */
	if (ptype == PREWARM_PREFETCH)
	{
#ifdef USE_PREFETCH

		/*
		 * In prefetch mode, we just hint the OS to read the blocks, but we
		 * don't know whether it really does it, and we don't wait for it to
		 * finish.
		 *
		 * It would probably be better to pass our prefetch requests in chunks
		 * of a megabyte or maybe even a whole segment at a time, but there's
		 * no practical way to do that at present without a gross modularity
		 * violation, so we just do this.
		 */
		for (block = first_block; block <= last_block; ++block)
		{
			CHECK_FOR_INTERRUPTS();
			if (blocks_done > 0 &&
				blocks_done % PREWARM_WAITER_CHECK_INTERVAL == 0)
				rel = pg_prewarm_check_and_yield(rel, relOid, privOid,
												 forkNumber, block,
												 blocks_done, &last_block);
			PrefetchBuffer(rel, forkNumber, block);
			++blocks_done;
		}
#else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("prefetch is not supported by this build")));
#endif
	}
	else if (ptype == PREWARM_READ)
	{
		/*
		 * In read mode, we actually read the blocks, but not into shared
		 * buffers.  This is more portable than prefetch mode (it works
		 * everywhere) and is synchronous.
		 */
		for (block = first_block; block <= last_block; ++block)
		{
			CHECK_FOR_INTERRUPTS();
			if (blocks_done > 0 &&
				blocks_done % PREWARM_WAITER_CHECK_INTERVAL == 0)
				rel = pg_prewarm_check_and_yield(rel, relOid, privOid,
												 forkNumber, block,
												 blocks_done, &last_block);
			smgrread(RelationGetSmgr(rel), forkNumber, block, blockbuffer.data);
			++blocks_done;
		}
	}
	else if (ptype == PREWARM_BUFFER)
	{
		BlockRangeReadStreamPrivate p;
		ReadStream *stream;

		/*
		 * In buffer mode, we actually pull the data into shared_buffers.
		 */

		/* Set up the private state for our streaming buffer read callback. */
		p.current_blocknum = first_block;
		p.last_exclusive = last_block + 1;

		/*
		 * It is safe to use batchmode as block_range_read_stream_cb takes no
		 * locks.
		 */
		stream = read_stream_begin_relation(READ_STREAM_MAINTENANCE |
											READ_STREAM_FULL |
											READ_STREAM_USE_BATCHING,
											NULL,
											rel,
											forkNumber,
											block_range_read_stream_cb,
											&p,
											0);

		for (block = first_block; block <= last_block; ++block)
		{
			Buffer		buf;

			CHECK_FOR_INTERRUPTS();
			buf = read_stream_next_buffer(stream, NULL);
			ReleaseBuffer(buf);
			++blocks_done;

			/*
			 * Periodically check for conflicting lock waiters.  If found, end
			 * the current read stream and yield, then start a new stream for
			 * the remaining blocks.
			 */
			if (blocks_done % PREWARM_WAITER_CHECK_INTERVAL == 0 &&
				block < last_block)
			{
				read_stream_end(stream);
				rel = pg_prewarm_check_and_yield(rel, relOid, privOid,
												 forkNumber, block + 1,
												 blocks_done, &last_block);

				/* Restart stream for remaining blocks. */
				p.current_blocknum = block + 1;
				p.last_exclusive = last_block + 1;
				stream = read_stream_begin_relation(READ_STREAM_MAINTENANCE |
													READ_STREAM_FULL |
													READ_STREAM_USE_BATCHING,
													NULL,
													rel,
													forkNumber,
													block_range_read_stream_cb,
													&p,
													0);
			}
		}
		Assert(read_stream_next_buffer(stream, NULL) == InvalidBuffer);
		read_stream_end(stream);
	}

	/* Close relation, release locks. */
	relation_close(rel, AccessShareLock);

	if (privOid != relOid)
		UnlockRelationOid(privOid, AccessShareLock);

	PG_RETURN_INT64(blocks_done);
}
