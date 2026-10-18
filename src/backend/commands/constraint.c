/*-------------------------------------------------------------------------
 *
 * constraint.c
 *	  PostgreSQL CONSTRAINT support code.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/constraint.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "utils/fmgrprotos.h"
#include "utils/snapmgr.h"


/*
 * Follow the updates of the row whose version is at *tid to its version
 * visible to SnapshotSelf, if any: set *tid to it, store it in slot, and
 * return true.  Returns false if there is none.
 */
static bool
row_live_version(Relation rel, ItemPointer tid, TupleTableSlot *slot)
{
	TableScanDesc scan = table_beginscan_tid(rel, SnapshotSelf);
	ItemPointerData latest = *tid;
	bool		live;

	table_tuple_get_latest_tid(scan, &latest);
	table_endscan(scan);
	live = table_tuple_fetch_row_version(rel, &latest, SnapshotSelf, slot);
	if (live)
		*tid = latest;
	return live;
}

/*
 * unique_key_recheck - trigger function to do a deferred uniqueness check.
 *
 * This now also does deferred exclusion-constraint checks, so the name is
 * somewhat historical.
 *
 * This is invoked as an AFTER ROW trigger for both INSERT and UPDATE,
 * for any rows recorded as potentially violating a deferrable unique
 * or exclusion constraint.
 *
 * This may be an end-of-statement check, a commit-time check, or a
 * check triggered by a SET CONSTRAINTS command.
 */
Datum
unique_key_recheck(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	const char *funcname = "unique_key_recheck";
	ItemPointerData checktid;
	ItemPointerData tmptid;
	Relation	indexRel;
	IndexInfo  *indexInfo;
	EState	   *estate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	TupleTableSlot *liveslot = NULL;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	/*
	 * Make sure this is being called as an AFTER ROW trigger.  Note:
	 * translatable error strings are shared with ri_triggers.c, so resist the
	 * temptation to fold the function name into them.
	 */
	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" was not called by trigger manager",
						funcname)));

	if (!TRIGGER_FIRED_AFTER(trigdata->tg_event) ||
		!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" must be fired AFTER ROW",
						funcname)));

	/*
	 * Get the new data that was inserted/updated.
	 */
	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		slot = trigdata->tg_trigslot;
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		slot = trigdata->tg_newslot;
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" must be fired for INSERT or UPDATE",
						funcname)));
		slot = NULL;			/* keep compiler quiet */
	}
	checktid = slot->tts_tid;

	/*
	 * If the row pointed at by checktid is now dead (ie, inserted and then
	 * deleted within our transaction), we can skip the check.  However, we
	 * have to be careful, because this trigger gets queued only in response
	 * to index insertions; which means it does not get queued e.g. for HOT
	 * updates.  The row we are called for might now be dead, but have a live
	 * HOT child, in which case we still need to make the check ---
	 * effectively, we're applying the check against the live child row,
	 * although we can use the values from this row since by definition all
	 * columns of interest to us are the same.
	 *
	 * This might look like just an optimization, because the index AM will
	 * make this identical test before throwing an error.  But it's actually
	 * needed for correctness, because the index AM will also throw an error
	 * if it doesn't find the index entry for the row.  If the row's dead then
	 * it's possible the index entry has also been marked dead, and even
	 * removed.
	 *
	 * An exclusion constraint's values need not be the same along the chain,
	 * though: a HOT update keeps the index's entries when its access method
	 * says the new values give the same ones, and then checks the constraint
	 * itself, queueing this recheck for the new version if it is deferred
	 * (see ExecCheckHotExclusion()).  That version is a heap-only tuple,
	 * which table_fetch_tid() cannot start from, and a later update may have
	 * changed the values again.  So for an exclusion constraint, follow the
	 * row's updates to the live version and check its values.
	 *
	 * Open the index first, acquiring a RowExclusiveLock, just as if we were
	 * going to update it.  (This protects against possible changes of the
	 * index schema, not against concurrent updates.)
	 */
	indexRel = index_open(trigdata->tg_trigger->tgconstrindid,
						  RowExclusiveLock);
	indexInfo = BuildIndexInfo(indexRel);

	tmptid = checktid;
	if (indexInfo->ii_ExclusionOps != NULL)
	{
		liveslot = table_slot_create(trigdata->tg_relation, NULL);
		if (!row_live_version(trigdata->tg_relation, &tmptid, liveslot))
		{
			ExecDropSingleTupleTableSlot(liveslot);
			index_close(indexRel, RowExclusiveLock);
			return PointerGetDatum(NULL);
		}
		slot = liveslot;
	}
	else if (!table_fetch_tid(trigdata->tg_relation, &tmptid, SnapshotSelf,
							  NULL))
	{
		/*
		 * All rows referenced by the index entry are dead, so skip the check
		 */
		index_close(indexRel, RowExclusiveLock);
		return PointerGetDatum(NULL);
	}

	/*
	 * Typically the index won't have expressions, but if it does we need an
	 * EState to evaluate them.  We need it for exclusion constraints too,
	 * even if they are just on simple columns.
	 */
	if (indexInfo->ii_Expressions != NIL ||
		indexInfo->ii_ExclusionOps != NULL)
	{
		estate = CreateExecutorState();
		econtext = GetPerTupleExprContext(estate);
		econtext->ecxt_scantuple = slot;
	}
	else
		estate = NULL;

	/*
	 * Form the index values and isnull flags for the index entry that we need
	 * to check.
	 *
	 * Note: if the index uses functions that are not as immutable as they are
	 * supposed to be, this could produce an index tuple different from the
	 * original.  The index AM can catch such errors by verifying that it
	 * finds a matching index entry with the tuple's TID.  For exclusion
	 * constraints we check this in check_exclusion_constraint().
	 */
	FormIndexDatum(indexInfo, slot, estate, values, isnull);

	/*
	 * Now do the appropriate check.
	 */
	if (indexInfo->ii_ExclusionOps == NULL)
	{
		/*
		 * Note: this is not a real insert; it is a check that the index entry
		 * that has already been inserted is unique.  Passing the tuple's tid
		 * (i.e. unmodified by table_fetch_tid()) is correct even if the row
		 * is now dead, because that is the TID the index will know about.
		 */
		index_insert(indexRel, values, isnull, &checktid,
					 trigdata->tg_relation, UNIQUE_CHECK_EXISTING,
					 false, indexInfo);

		/* Cleanup cache possibly initialized by index_insert. */
		index_insert_cleanup(indexRel, indexInfo);
	}
	else
	{
		/*
		 * For exclusion constraints we just do the normal check, but now it's
		 * okay to throw error.  In the HOT-update case, we must use the live
		 * HOT child's TID here, else check_exclusion_constraint will think
		 * the child is a conflict.
		 */
		check_exclusion_constraint(trigdata->tg_relation, indexRel, indexInfo,
								   &tmptid, values, isnull,
								   estate, false);
	}

	/*
	 * If that worked, then this index entry is unique or non-excluded, and we
	 * are done.
	 */
	if (estate != NULL)
		FreeExecutorState(estate);
	if (liveslot != NULL)
		ExecDropSingleTupleTableSlot(liveslot);

	index_close(indexRel, RowExclusiveLock);

	return PointerGetDatum(NULL);
}
