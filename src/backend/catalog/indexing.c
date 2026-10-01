/*-------------------------------------------------------------------------
 *
 * indexing.c
 *	  This file contains routines to support indexes defined on system
 *	  catalogs.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/indexing.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"


/*
 * CatalogOpenIndexes - open the indexes on a system catalog.
 *
 * When inserting or updating tuples in a system catalog, call this
 * to prepare to update the indexes for the catalog.
 *
 * In the current implementation, we share code for opening/closing the
 * indexes with execUtils.c.  But we do not use ExecInsertIndexTuples,
 * because we don't want to create an EState.  This implies that we
 * do not support partial or expressional indexes on system catalogs,
 * nor can we support generalized exclusion constraints.
 * This could be fixed with localized changes here if we wanted to pay
 * the extra overhead of building an EState.
 */
CatalogIndexState
CatalogOpenIndexes(Relation heapRel)
{
	ResultRelInfo *resultRelInfo;

	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 0;	/* dummy */
	resultRelInfo->ri_RelationDesc = heapRel;
	resultRelInfo->ri_TrigDesc = NULL;	/* we don't fire triggers */

	ExecOpenIndices(resultRelInfo, false);

	return resultRelInfo;
}

/*
 * CatalogCloseIndexes - clean up resources allocated by CatalogOpenIndexes
 */
void
CatalogCloseIndexes(CatalogIndexState indstate)
{
	ExecCloseIndices(indstate);
	pfree(indstate);
}

/*
 * catalog_pred_clause_supported - is this predicate clause one we can maintain?
 *
 * Only a tiny whitelist of clause shapes can be evaluated during catalog
 * maintenance without building an EState: an OID inequality "oidcol <> k" and
 * "col IS NOT NULL".  Shared by the creation-time check and the maintenance
 * evaluator so the two can never drift apart.
 */
static bool
catalog_pred_clause_supported(Node *clause)
{
	if (IsA(clause, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) clause;

		if (op->opfuncid != F_OIDNE || list_length(op->args) != 2)
			return false;
		/* literals arrive wrapped in an implicit int4->oid cast */
		return (IsA(strip_implicit_coercions((Node *) linitial(op->args)), Var) &&
				IsA(strip_implicit_coercions((Node *) lsecond(op->args)), Const));
	}
	if (IsA(clause, NullTest))
	{
		NullTest   *nt = (NullTest *) clause;

		return (nt->nulltesttype == IS_NOT_NULL && IsA(nt->arg, Var));
	}
	return false;
}

/*
 * CheckCatalogIndexPredicate - reject predicates we could not maintain
 *
 * Called at index creation time for any partial index on a system catalog, so
 * that such an index can never be built carrying a predicate that
 * CatalogIndexInsert would be unable to evaluate later.
 */
void
CheckCatalogIndexPredicate(List *predicate)
{
	ListCell   *lc;

	foreach(lc, predicate)
	{
		if (!catalog_pred_clause_supported((Node *) lfirst(lc)))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported predicate for a partial index on a system catalog"),
					 errdetail("Only \"oidcolumn <> 0\" and \"column IS NOT NULL\" predicates are supported.")));
	}
}

/*
 * CatalogIndexPredSatisfied - evaluate a restricted partial-index predicate
 *
 * Evaluates the predicate against the tuple in "slot" without building an
 * EState.  Only the shapes accepted by catalog_pred_clause_supported() can
 * occur here (enforced at creation by CheckCatalogIndexPredicate); anything
 * else is a should-not-happen, so we fail loudly rather than guess.
 *
 * The predicate is an implicit-AND list of clauses.
 */
static bool
CatalogIndexPredSatisfied(List *predicate, TupleTableSlot *slot)
{
	ListCell   *lc;

	foreach(lc, predicate)
	{
		Node	   *clause = (Node *) lfirst(lc);
		Var		   *var;
		Datum		val;
		bool		isnull;

		if (!catalog_pred_clause_supported(clause))
			elog(ERROR, "unsupported predicate on system catalog index");

		if (IsA(clause, OpExpr))
		{
			OpExpr	   *op = (OpExpr *) clause;
			Const	   *con;

			var = (Var *) strip_implicit_coercions((Node *) linitial(op->args));
			con = (Const *) strip_implicit_coercions((Node *) lsecond(op->args));
			val = slot_getattr(slot, var->varattno, &isnull);
			if (isnull || con->constisnull)
				return false;
			if (DatumGetObjectId(val) == DatumGetObjectId(con->constvalue))
				return false;	/* "<>" fails when the values are equal */
		}
		else					/* NullTest IS NOT NULL */
		{
			var = (Var *) ((NullTest *) clause)->arg;
			(void) slot_getattr(slot, var->varattno, &isnull);
			if (isnull)
				return false;
		}
	}

	return true;
}

/*
 * CatalogIndexInsert - insert index entries for one catalog tuple
 *
 * This should be called for each inserted or updated catalog tuple.
 *
 * This is effectively a cut-down version of ExecInsertIndexTuples.
 */
static void
CatalogIndexInsert(CatalogIndexState indstate, HeapTuple heapTuple,
				   TU_UpdateIndexes updateIndexes)
{
	int			i;
	int			numIndexes;
	RelationPtr relationDescs;
	Relation	heapRelation;
	TupleTableSlot *slot;
	IndexInfo **indexInfoArray;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	bool		onlySummarized = (updateIndexes == TU_Summarizing);

	/*
	 * HOT update does not require index inserts. But with asserts enabled we
	 * want to check that it'd be legal to currently insert into the
	 * table/index.
	 */
#ifndef USE_ASSERT_CHECKING
	if (HeapTupleIsHeapOnly(heapTuple) && !onlySummarized)
		return;
#endif

	/* When only updating summarized indexes, the tuple has to be HOT. */
	Assert((!onlySummarized) || HeapTupleIsHeapOnly(heapTuple));

	/*
	 * Get information from the state structure.  Fall out if nothing to do.
	 */
	numIndexes = indstate->ri_NumIndices;
	if (numIndexes == 0)
		return;
	relationDescs = indstate->ri_IndexRelationDescs;
	indexInfoArray = indstate->ri_IndexRelationInfo;
	heapRelation = indstate->ri_RelationDesc;

	/* Need a slot to hold the tuple being examined */
	slot = MakeSingleTupleTableSlot(RelationGetDescr(heapRelation),
									&TTSOpsHeapTuple);
	ExecStoreHeapTuple(heapTuple, slot, false);

	/*
	 * for each index, form and insert the index tuple
	 */
	for (i = 0; i < numIndexes; i++)
	{
		IndexInfo  *indexInfo;
		Relation	index;

		indexInfo = indexInfoArray[i];
		index = relationDescs[i];

		/* If the index is marked as read-only, ignore it */
		if (!indexInfo->ii_ReadyForInserts)
			continue;

		/*
		 * Expressional indexes on system catalogs are not supported, nor
		 * exclusion constraints, nor deferred uniqueness.  Partial indexes
		 * are supported for a restricted set of predicates, evaluated below
		 * by CatalogIndexPredSatisfied without building an EState.
		 */
		Assert(indexInfo->ii_Expressions == NIL);
		Assert(indexInfo->ii_ExclusionOps == NULL);
		Assert(index->rd_index->indimmediate);
		Assert(indexInfo->ii_NumIndexKeyAttrs != 0);

		/* see earlier check above */
#ifdef USE_ASSERT_CHECKING
		if (HeapTupleIsHeapOnly(heapTuple) && !onlySummarized)
		{
			Assert(!ReindexIsProcessingIndex(RelationGetRelid(index)));
			continue;
		}
#endif							/* USE_ASSERT_CHECKING */

		/*
		 * Skip insertions into non-summarizing indexes if we only need to
		 * update summarizing indexes.
		 */
		if (onlySummarized && !indexInfo->ii_Summarizing)
			continue;

		/*
		 * For a partial index, skip tuples that don't satisfy the predicate.
		 * We evaluate it without an executor; only a whitelisted set of
		 * predicate shapes is allowed (see CatalogIndexPredSatisfied).
		 */
		if (indexInfo->ii_Predicate != NIL &&
			!CatalogIndexPredSatisfied(indexInfo->ii_Predicate, slot))
			continue;

		/*
		 * FormIndexDatum fills in its values and isnull parameters with the
		 * appropriate values for the column(s) of the index.
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   NULL,	/* no expression eval to do */
					   values,
					   isnull);

		/*
		 * The index AM does the rest.
		 */
		index_insert(index,		/* index relation */
					 values,	/* array of index Datums */
					 isnull,	/* is-null flags */
					 &(heapTuple->t_self),	/* tid of heap tuple */
					 heapRelation,
					 index->rd_index->indisunique ?
					 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
					 false,
					 indexInfo);
	}

	ExecDropSingleTupleTableSlot(slot);
}

/*
 * Subroutine to verify that catalog constraints are honored.
 *
 * Tuples inserted via CatalogTupleInsert/CatalogTupleUpdate are generally
 * "hand made", so that it's possible that they fail to satisfy constraints
 * that would be checked if they were being inserted by the executor.  That's
 * a coding error, so we only bother to check for it in assert-enabled builds.
 */
#ifdef USE_ASSERT_CHECKING

static void
CatalogTupleCheckConstraints(Relation heapRel, HeapTuple tup)
{
	/*
	 * Currently, the only constraints implemented for system catalogs are
	 * attnotnull constraints.
	 */
	if (HeapTupleHasNulls(tup))
	{
		TupleDesc	tupdesc = RelationGetDescr(heapRel);
		uint8	   *bp = tup->t_data->t_bits;

		for (int attnum = 0; attnum < tupdesc->natts; attnum++)
		{
			Form_pg_attribute thisatt = TupleDescAttr(tupdesc, attnum);

			Assert(!(thisatt->attnotnull && att_isnull(attnum, bp)));
		}
	}
}

#else							/* !USE_ASSERT_CHECKING */

#define CatalogTupleCheckConstraints(heapRel, tup)  ((void) 0)

#endif							/* USE_ASSERT_CHECKING */

/*
 * CatalogTupleInsert - do heap and indexing work for a new catalog tuple
 *
 * Insert the tuple data in "tup" into the specified catalog relation.
 *
 * This is a convenience routine for the common case of inserting a single
 * tuple in a system catalog; it inserts a new heap tuple, keeping indexes
 * current.  Avoid using it for multiple tuples, since opening the indexes
 * and building the index info structures is moderately expensive.
 * (Use CatalogTupleInsertWithInfo in such cases.)
 */
void
CatalogTupleInsert(Relation heapRel, HeapTuple tup)
{
	CatalogIndexState indstate;

	CatalogTupleCheckConstraints(heapRel, tup);

	indstate = CatalogOpenIndexes(heapRel);

	simple_heap_insert(heapRel, tup);

	CatalogIndexInsert(indstate, tup, TU_All);
	CatalogCloseIndexes(indstate);
}

/*
 * CatalogTupleInsertWithInfo - as above, but with caller-supplied index info
 *
 * This should be used when it's important to amortize CatalogOpenIndexes/
 * CatalogCloseIndexes work across multiple insertions.  At some point we
 * might cache the CatalogIndexState data somewhere (perhaps in the relcache)
 * so that callers needn't trouble over this ... but we don't do so today.
 */
void
CatalogTupleInsertWithInfo(Relation heapRel, HeapTuple tup,
						   CatalogIndexState indstate)
{
	CatalogTupleCheckConstraints(heapRel, tup);

	simple_heap_insert(heapRel, tup);

	CatalogIndexInsert(indstate, tup, TU_All);
}

/*
 * CatalogTuplesMultiInsertWithInfo - as above, but for multiple tuples
 *
 * Insert multiple tuples into the given catalog relation at once, with an
 * amortized cost of CatalogOpenIndexes.
 */
void
CatalogTuplesMultiInsertWithInfo(Relation heapRel, TupleTableSlot **slot,
								 int ntuples, CatalogIndexState indstate)
{
	/* Nothing to do */
	if (ntuples <= 0)
		return;

	heap_multi_insert(heapRel, slot, ntuples,
					  GetCurrentCommandId(true), 0, NULL);

	/*
	 * There is no equivalent to heap_multi_insert for the catalog indexes, so
	 * we must loop over and insert individually.
	 */
	for (int i = 0; i < ntuples; i++)
	{
		bool		should_free;
		HeapTuple	tuple;

		tuple = ExecFetchSlotHeapTuple(slot[i], true, &should_free);
		tuple->t_tableOid = slot[i]->tts_tableOid;
		CatalogIndexInsert(indstate, tuple, TU_All);

		if (should_free)
			heap_freetuple(tuple);
	}
}

/*
 * CatalogTupleUpdate - do heap and indexing work for updating a catalog tuple
 *
 * Update the tuple identified by "otid", replacing it with the data in "tup".
 *
 * This is a convenience routine for the common case of updating a single
 * tuple in a system catalog; it updates one heap tuple, keeping indexes
 * current.  Avoid using it for multiple tuples, since opening the indexes
 * and building the index info structures is moderately expensive.
 * (Use CatalogTupleUpdateWithInfo in such cases.)
 */
void
CatalogTupleUpdate(Relation heapRel, const ItemPointerData *otid, HeapTuple tup)
{
	CatalogIndexState indstate;
	TU_UpdateIndexes updateIndexes = TU_All;

	CatalogTupleCheckConstraints(heapRel, tup);

	indstate = CatalogOpenIndexes(heapRel);

	simple_heap_update(heapRel, otid, tup, &updateIndexes);

	CatalogIndexInsert(indstate, tup, updateIndexes);
	CatalogCloseIndexes(indstate);
}

/*
 * CatalogTupleUpdateWithInfo - as above, but with caller-supplied index info
 *
 * This should be used when it's important to amortize CatalogOpenIndexes/
 * CatalogCloseIndexes work across multiple updates.  At some point we
 * might cache the CatalogIndexState data somewhere (perhaps in the relcache)
 * so that callers needn't trouble over this ... but we don't do so today.
 */
void
CatalogTupleUpdateWithInfo(Relation heapRel, const ItemPointerData *otid, HeapTuple tup,
						   CatalogIndexState indstate)
{
	TU_UpdateIndexes updateIndexes = TU_All;

	CatalogTupleCheckConstraints(heapRel, tup);

	simple_heap_update(heapRel, otid, tup, &updateIndexes);

	CatalogIndexInsert(indstate, tup, updateIndexes);
}

/*
 * CatalogTupleDelete - do heap and indexing work for deleting a catalog tuple
 *
 * Delete the tuple identified by "tid" in the specified catalog.
 *
 * With Postgres heaps, there is no index work to do at deletion time;
 * cleanup will be done later by VACUUM.  However, callers of this function
 * shouldn't have to know that; we'd like a uniform abstraction for all
 * catalog tuple changes.  Hence, provide this currently-trivial wrapper.
 *
 * The abstraction is a bit leaky in that we don't provide an optimized
 * CatalogTupleDeleteWithInfo version, because there is currently nothing to
 * optimize.  If we ever need that, rather than touching a lot of call sites,
 * it might be better to do something about caching CatalogIndexState.
 */
void
CatalogTupleDelete(Relation heapRel, const ItemPointerData *tid)
{
	simple_heap_delete(heapRel, tid);
}
