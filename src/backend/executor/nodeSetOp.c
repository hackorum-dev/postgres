/*-------------------------------------------------------------------------
 *
 * nodeSetOp.c
 *	  Routines to handle INTERSECT and EXCEPT selection
 *
 * The input of a SetOp node consists of tuples from two relations,
 * which have been combined into one dataset, with a junk attribute added
 * that shows which relation each tuple came from.  In SETOP_SORTED mode,
 * the input has furthermore been sorted according to all the grouping
 * columns (ie, all the non-junk attributes).  The SetOp node scans each
 * group of identical tuples to determine how many came from each input
 * relation.  Then it is a simple matter to emit the output demanded by the
 * SQL spec for INTERSECT, INTERSECT ALL, EXCEPT, or EXCEPT ALL.
 *
 * In SETOP_HASHED mode, the input is delivered in no particular order,
 * except that we know all the tuples from one input relation will come before
 * all the tuples of the other.  The planner guarantees that the first input
 * relation is the left-hand one for EXCEPT, and tries to make the smaller
 * input relation come first for INTERSECT.  We build a hash table in memory
 * with one entry for each group of identical tuples, and count the number of
 * tuples in the group from each relation.  After seeing all the input, we
 * scan the hashtable and generate the correct output using those counts.
 * We can avoid making hashtable entries for any tuples appearing only in the
 * second input relation, since they cannot result in any output.
 * (XXX: except that we do, if the hash table has been spilled)
 *
 * This node type is not used for UNION or UNION ALL, since those can be
 * implemented more cheaply (there's no need for the junk attribute to
 * identify the source relation).
 *
 * Note that SetOp does no qual checking nor projection.  The delivered
 * output tuples are just copies of the first-to-arrive tuple in each
 * input group.
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSetOp.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "executor/executor.h"
#include "executor/nodeSetOp.h"
#include "miscadmin.h"
#include "utils/memutils.h"


#define NUM_PERGROUPS_PER_ALLOC 100

/*
 * SetOpStatePerGroupData - per-group working state
 *
 * These values are working state that is initialized at the start of
 * an input tuple group and updated for each input tuple.
 *
 * In SETOP_SORTED mode, we need only one of these structs, and it's kept in
 * the plan state node.  In SETOP_HASHED mode, the hash table contains one
 * of these for each tuple group.
 *
 * Unused per-group structs are kept in a linked list, in
 * SetOpState.free_pergroups.  In that case, 'next' points to the next struct
 * in the free-list.
 */
typedef union SetOpStatePerGroupData
{
	struct
	{
		uint64		numLeft;		/* number of left-input dups in group */
		uint64		numRight;		/* number of right-input dups in group */
	} data;
	SetOpStatePerGroup next;		/* next unused entry in the free list */
}			SetOpStatePerGroupData;


static TupleTableSlot *setop_retrieve_direct(SetOpState *setopstate);
static void setop_fill_hash_table(SetOpState *setopstate);
static TupleTableSlot *setop_retrieve_hash_table(SetOpState *setopstate);

static void setop_spill_entry(SetOpState *setopstate, TupleHashEntry entry, bool in_hashtab);

/*
 * Initialize state for a new group of input values.
 */
static inline void
initialize_counts(SetOpStatePerGroup pergroup)
{
	pergroup->data.numLeft = pergroup->data.numRight = 0;
}

/*
 * Advance the appropriate counter for one input tuple.
 */
static inline void
advance_counts(SetOpStatePerGroup pergroup, int flag)
{
	if (flag)
		pergroup->data.numRight++;
	else
		pergroup->data.numLeft++;
}

/*
 * Allocate a new per-group struct.
 *
 * To save on memory and palloc() call overhead, we allocate the per-group
 * structs in batches.
 */
static SetOpStatePerGroup
alloc_pergroup(SetOpState *setopstate)
{
	SetOpStatePerGroup pergroup;

	if (!setopstate->free_pergroups)
	{
		int			i;

		setopstate->free_pergroups =
			MemoryContextAlloc(setopstate->hashtable->tablecxt,
							   NUM_PERGROUPS_PER_ALLOC * sizeof(SetOpStatePerGroupData));

		for (i = 0; i < NUM_PERGROUPS_PER_ALLOC - 1; i++)
			setopstate->free_pergroups[i].next = &setopstate->free_pergroups[i + 1];
		setopstate->free_pergroups[NUM_PERGROUPS_PER_ALLOC - 1].next = NULL;
	}

	pergroup = setopstate->free_pergroups;
	setopstate->free_pergroups = pergroup->next;

	return pergroup;
}

static void
free_pergroup(SetOpState *setopstate, SetOpStatePerGroup pergroup)
{
	pergroup->next = setopstate->free_pergroups;
	setopstate->free_pergroups = pergroup;
}

/*
 * Fetch the "flag" column from an input tuple.
 * This is an integer column with value 0 for left side, 1 for right side.
 */
static int
fetch_tuple_flag(SetOpState *setopstate, TupleTableSlot *inputslot)
{
	SetOp	   *node = (SetOp *) setopstate->ps.plan;
	int			flag;
	bool		isNull;

	flag = DatumGetInt32(slot_getattr(inputslot,
									  node->flagColIdx,
									  &isNull));
	Assert(!isNull);
	Assert(flag == 0 || flag == 1);
	return flag;
}

/*
 * Initialize the hash table to empty.
 */
static void
build_hash_table(SetOpState *setopstate)
{
	SetOp	   *node = (SetOp *) setopstate->ps.plan;
	ExprContext *econtext = setopstate->ps.ps_ExprContext;
	TupleDesc	desc = ExecGetResultType(outerPlanState(setopstate));

	Assert(node->strategy == SETOP_HASHED);
	Assert(node->numGroups > 0);

	setopstate->hashtable = BuildTupleHashTable(&setopstate->ps,
												desc,
												node->numCols,
												node->dupColIdx,
												setopstate->eqfuncoids,
												setopstate->hashfunctions,
												node->numGroups,
												0,
												setopstate->tableContext,
												econtext->ecxt_per_tuple_memory,
												false);
	setopstate->hashtable->allowedMem = work_mem * 1024L;
}

/*
 * We've completed processing a tuple group.  Decide how many copies (if any)
 * of its representative row to emit, and store the count into numOutput.
 * This logic is straight from the SQL92 specification.
 */
static void
set_output_count(SetOpState *setopstate, SetOpStatePerGroup pergroup)
{
	SetOp	   *plannode = (SetOp *) setopstate->ps.plan;

	switch (plannode->cmd)
	{
		case SETOPCMD_INTERSECT:
			if (pergroup->data.numLeft > 0 && pergroup->data.numRight > 0)
				setopstate->numOutput = 1;
			else
				setopstate->numOutput = 0;
			break;
		case SETOPCMD_INTERSECT_ALL:
			setopstate->numOutput =
				(pergroup->data.numLeft < pergroup->data.numRight) ?
				pergroup->data.numLeft : pergroup->data.numRight;
			break;
		case SETOPCMD_EXCEPT:
			if (pergroup->data.numLeft > 0 && pergroup->data.numRight == 0)
				setopstate->numOutput = 1;
			else
				setopstate->numOutput = 0;
			break;
		case SETOPCMD_EXCEPT_ALL:
			setopstate->numOutput =
				(pergroup->data.numLeft < pergroup->data.numRight) ?
				0 : (pergroup->data.numLeft - pergroup->data.numRight);
			break;
		default:
			elog(ERROR, "unrecognized set op: %d", (int) plannode->cmd);
			break;
	}
}


/* ----------------------------------------------------------------
 *		ExecSetOp
 * ----------------------------------------------------------------
 */
static TupleTableSlot *			/* return: a tuple or NULL */
ExecSetOp(PlanState *pstate)
{
	SetOpState *node = castNode(SetOpState, pstate);
	SetOp	   *plannode = (SetOp *) node->ps.plan;
	TupleTableSlot *resultTupleSlot = node->ps.ps_ResultTupleSlot;

	CHECK_FOR_INTERRUPTS();

	/*
	 * If the previously-returned tuple needs to be returned more than once,
	 * keep returning it.
	 */
	if (node->numOutput > 0)
	{
		node->numOutput--;
		return resultTupleSlot;
	}

	/* Otherwise, we're done if we are out of groups */
	if (node->setop_done)
		return NULL;

	/* Fetch the next tuple group according to the correct strategy */
	if (plannode->strategy == SETOP_HASHED)
	{
		if (!node->table_filled)
			setop_fill_hash_table(node);
		return setop_retrieve_hash_table(node);
	}
	else
		return setop_retrieve_direct(node);
}

/*
 * ExecSetOp for non-hashed case
 */
static TupleTableSlot *
setop_retrieve_direct(SetOpState *setopstate)
{
	PlanState  *outerPlan;
	SetOpStatePerGroup pergroup;
	TupleTableSlot *outerslot;
	TupleTableSlot *resultTupleSlot;
	ExprContext *econtext = setopstate->ps.ps_ExprContext;

	/*
	 * get state info from node
	 */
	outerPlan = outerPlanState(setopstate);
	pergroup = (SetOpStatePerGroup) setopstate->pergroup;
	resultTupleSlot = setopstate->ps.ps_ResultTupleSlot;

	/*
	 * We loop retrieving groups until we find one we should return
	 */
	while (!setopstate->setop_done)
	{
		/*
		 * If we don't already have the first tuple of the new group, fetch it
		 * from the outer plan.
		 */
		if (setopstate->grp_firstTuple == NULL)
		{
			outerslot = ExecProcNode(outerPlan);
			if (!TupIsNull(outerslot))
			{
				/* Make a copy of the first input tuple */
				setopstate->grp_firstTuple = ExecCopySlotTuple(outerslot);
			}
			else
			{
				/* outer plan produced no tuples at all */
				setopstate->setop_done = true;
				return NULL;
			}
		}

		/*
		 * Store the copied first input tuple in the tuple table slot reserved
		 * for it.  The tuple will be deleted when it is cleared from the
		 * slot.
		 */
		ExecStoreTuple(setopstate->grp_firstTuple,
					   resultTupleSlot,
					   InvalidBuffer,
					   true);
		setopstate->grp_firstTuple = NULL;	/* don't keep two pointers */

		/* Initialize working state for a new input tuple group */
		initialize_counts(pergroup);

		/* Count the first input tuple */
		advance_counts(pergroup,
					   fetch_tuple_flag(setopstate, resultTupleSlot));

		/*
		 * Scan the outer plan until we exhaust it or cross a group boundary.
		 */
		for (;;)
		{
			outerslot = ExecProcNode(outerPlan);
			if (TupIsNull(outerslot))
			{
				/* no more outer-plan tuples available */
				setopstate->setop_done = true;
				break;
			}

			/*
			 * Check whether we've crossed a group boundary.
			 */
			econtext->ecxt_outertuple = resultTupleSlot;
			econtext->ecxt_innertuple = outerslot;

			if (!ExecQualAndReset(setopstate->eqfunction, econtext))
			{
				/*
				 * Save the first input tuple of the next group.
				 */
				setopstate->grp_firstTuple = ExecCopySlotTuple(outerslot);
				break;
			}

			/* Still in same group, so count this tuple */
			advance_counts(pergroup,
						   fetch_tuple_flag(setopstate, outerslot));
		}

		/*
		 * Done scanning input tuple group.  See if we should emit any copies
		 * of result tuple, and if so return the first copy.
		 */
		set_output_count(setopstate, pergroup);

		if (setopstate->numOutput > 0)
		{
			setopstate->numOutput--;
			return resultTupleSlot;
		}
	}

	/* No more groups */
	ExecClearTuple(resultTupleSlot);
	return NULL;
}

static void
BufFileWriteNoError(BufFile *file, void *ptr, size_t size)
{
	if (BufFileWrite(file, ptr, size) != size)
		elog(ERROR, "io error"); /* XXX ereport */
}

/* Callback to dump the 'additional' data */
static void
setop_spill_entries(SetOpState *setopstate)
{
	if (setopstate->hashtable->usedMem < setopstate->hashtable->allowedMem)
		return;

	if (!setopstate->spillset)
	{
		setopstate->spillset = CreateHashSpillSet(setopstate->hashtable->allowedMem / 2);

		setopstate->spillslot = MakeSingleTupleTableSlot(CreateTupleDescCopy(ExecGetResultType(outerPlanState(setopstate))));
	}

	while (setopstate->hashtable->usedMem > setopstate->hashtable->allowedMem &&
		setopstate->hashtable->hashtab->members > 1)
	{
		TupleHashEntry entry;
#if 0
		elog(NOTICE, "spilling: used %ld allowed %ld tuples %d",
			 setopstate->hashtable->usedMem, setopstate->hashtable->allowedMem,
			setopstate->hashtable->hashtab->members);
#endif
		entry = SpillTupleHashTable(setopstate->hashtable);
		if (!entry)
			break;

		setop_spill_entry(setopstate, entry, true);
	}
}

static void
setop_spill_entry(SetOpState *setopstate, TupleHashEntry entry, bool in_hashtab)
{
	BufFile	   *file;
	MinimalTuple tup;

	file = GetSpillFile(setopstate->spillset, entry->hash);

	BufFileWriteNoError(file, &entry->hash, sizeof(uint32));
	BufFileWriteNoError(file, &entry->firstTuple->t_len, sizeof(uint32));
	BufFileWriteNoError(file, entry->firstTuple, entry->firstTuple->t_len);
	BufFileWriteNoError(file, entry->additional, sizeof(SetOpStatePerGroupData));

	if (in_hashtab)
	{
		/* Free the entry. */
		setopstate->hashtable->usedMem -= sizeof(SetOpStatePerGroupData);
		setopstate->hashtable->usedMem -= GetMemoryChunkSpace(entry->firstTuple);
		free_pergroup(setopstate, entry->additional);
		tup = entry->firstTuple;
		tuplehash_delete_elem(setopstate->hashtable->hashtab, entry, entry->hash);
		pfree(tup);

		if (!setopstate->spilled)
		{
			setopstate->spilled = true;

			elog(NOTICE, "spilling: used %ld allowed %ld tuples %d",
				 setopstate->hashtable->usedMem, setopstate->hashtable->allowedMem,
				 setopstate->hashtable->hashtab->members);
		}
	}
}

static void
BufFileReadExact(BufFile *file, void *ptr, size_t size)
{
	if (BufFileRead(file, ptr, size) != size)
		elog(ERROR, "error reading file");
}

/*
 * Reload next batch into memory.
 *
 * May cause further spilling. Returns false if there were no more batches.
 */
static bool
setop_reload_batch(SetOpState *setopstate)
{
	uint32		loaded_hash;
	uint32		tuplen;
	MinimalTuple loaded_firstTuple;
	SetOpStatePerGroup pergroup;
	SetOpStatePerGroupData loaded_pergroup;
	int			readlen;
	BufFile	   *file;
	TupleHashEntry entry;
	bool		isnew;
	bool		respill;

	Assert(setopstate->hashtable->hashtab->members == 0);
	setopstate->spilled = false;

	file = OpenNextSpillFile(setopstate->spillset, &respill);
	if (!file)
		return false;

	setopstate->spilled = false;

	for (;;)
	{
		readlen = BufFileRead(file, &loaded_hash, sizeof(uint32));
		if (readlen == 0)
		{
			//elog(NOTICE, "reloaded batch %p with %d tuples", file, setopstate->hashtable->hashtab->members);
			BufFileClose(file);
			break;
		}
		if (readlen != sizeof(uint32))
			elog(ERROR, "error reading file");
		BufFileReadExact(file, &tuplen, sizeof(uint32));
		loaded_firstTuple = palloc(tuplen);
		BufFileReadExact(file, loaded_firstTuple, tuplen);
		Assert(loaded_firstTuple->t_len == tuplen);
		BufFileReadExact(file, &loaded_pergroup, sizeof(SetOpStatePerGroupData));

		ExecStoreMinimalTuple(loaded_firstTuple, setopstate->spillslot, true);

		if (setopstate->hashtable->usedMem < setopstate->hashtable->allowedMem ||
			setopstate->hashtable->hashtab->members < 1)
		{
			entry = LookupTupleHashEntry(setopstate->hashtable, setopstate->spillslot,
										 &isnew);

			if (isnew)
			{
				entry->additional = alloc_pergroup(setopstate);
				setopstate->hashtable->usedMem += sizeof(SetOpStatePerGroupData);
				initialize_counts((SetOpStatePerGroup) entry->additional);
				if (setopstate->spilled || respill)
					entry->firstbatch = false;
				else
					entry->firstbatch = true;
			}

			pergroup = (SetOpStatePerGroup) entry->additional;
			pergroup->data.numLeft += loaded_pergroup.data.numLeft;
			pergroup->data.numRight += loaded_pergroup.data.numRight;
		}
		else
		{
			/*
			 * We are already over the limit. Check if it happens to be in the
			 * hash table, but if not, re-spill it immediately. This is
			 * important, so that we process entries in LIFO order. Otherwise,
			 * if there are enough entries with the same hash value to not fit
			 * in memory at the time, we might loop and not make any progress.
			 */
			entry = LookupTupleHashEntry(setopstate->hashtable,
										 setopstate->spillslot,
										 NULL);
			if (entry)
			{
				pergroup = (SetOpStatePerGroup) entry->additional;
				pergroup->data.numLeft += loaded_pergroup.data.numLeft;
				pergroup->data.numRight += loaded_pergroup.data.numRight;
			}
			else
			{
				TupleHashEntryData loaded_entry;

				loaded_entry.hash = loaded_hash;
				loaded_entry.firstTuple = loaded_firstTuple;
				loaded_entry.additional = &loaded_pergroup;

				setop_spill_entry(setopstate, &loaded_entry, false);
			}
		}
		ExecClearTuple(setopstate->spillslot);
	}

	return true;
}

/*
 * ExecSetOp for hashed case: phase 1, read input and build hash table
 */
static void
setop_fill_hash_table(SetOpState *setopstate)
{
	SetOp	   *node = (SetOp *) setopstate->ps.plan;
	PlanState  *outerPlan;
	int			firstFlag;
	bool		in_first_rel PG_USED_FOR_ASSERTS_ONLY;
	ExprContext *econtext = setopstate->ps.ps_ExprContext;

	/*
	 * get state info from node
	 */
	outerPlan = outerPlanState(setopstate);
	firstFlag = node->firstFlag;
	/* verify planner didn't mess up */
	Assert(firstFlag == 0 ||
		   (firstFlag == 1 &&
			(node->cmd == SETOPCMD_INTERSECT ||
			 node->cmd == SETOPCMD_INTERSECT_ALL)));

	/*
	 * Process each outer-plan tuple, and then fetch the next one, until we
	 * exhaust the outer plan.
	 */
	in_first_rel = true;
	for (;;)
	{
		TupleTableSlot *outerslot;
		int			flag;
		TupleHashEntryData *entry;
		bool		isnew;

		outerslot = ExecProcNode(outerPlan);
		if (TupIsNull(outerslot))
			break;

		/* Identify whether it's left or right input */
		flag = fetch_tuple_flag(setopstate, outerslot);

		/* If we're out of memory, spill some tuples from the hash table. */
		setop_spill_entries(setopstate);

		if (flag == firstFlag)
		{
			/* (still) in first input relation */
			Assert(in_first_rel);

			/* Find or build hashtable entry for this tuple's group */
			entry = LookupTupleHashEntry(setopstate->hashtable, outerslot,
										 &isnew);

			/* If new tuple group, initialize counts */
			if (isnew)
			{
				entry->additional = alloc_pergroup(setopstate);
				setopstate->hashtable->usedMem += sizeof(SetOpStatePerGroupData);
				initialize_counts((SetOpStatePerGroup) entry->additional);
				if (setopstate->spilled)
					entry->firstbatch = false;
				else
					entry->firstbatch = true;
			}

			/* Advance the counts */
			advance_counts((SetOpStatePerGroup) entry->additional, flag);
		}
		else
		{
			/* reached second relation */
			in_first_rel = false;

			/* For tuples not seen previously, do not make hashtable entry */
			/* If the hash table has already been spilled, we must make entries
			 * for everything, because we might have spilled an entry for this
			 * key already.
			 */
			isnew = false;
			entry = LookupTupleHashEntry(setopstate->hashtable, outerslot,
										 setopstate->spilled ? &isnew : NULL);

			/* Advance the counts if entry is already present */
			if (entry)
			{
				/* If new tuple group, initialize counts */
				if (isnew)
				{
					entry->additional = alloc_pergroup(setopstate);
					setopstate->hashtable->usedMem += sizeof(SetOpStatePerGroupData);
					initialize_counts((SetOpStatePerGroup) entry->additional);
					if (setopstate->spilled)
						entry->firstbatch = false;
					else
						entry->firstbatch = true;
				}
				advance_counts((SetOpStatePerGroup) entry->additional, flag);
			}
		}

		/* Must reset expression context after each hashtable lookup */
		ResetExprContext(econtext);
	}

	setopstate->table_filled = true;
	/* Initialize to walk the hash table */
	ResetTupleHashIterator(setopstate->hashtable, &setopstate->hashiter);

	elog(NOTICE, "table filled");
}

/*
 * ExecSetOp for hashed case: phase 2, retrieving groups from hash table
 */
static TupleTableSlot *
setop_retrieve_hash_table(SetOpState *setopstate)
{
	TupleHashEntryData *entry;
	TupleTableSlot *resultTupleSlot;

	/*
	 * get state info from node
	 */
	resultTupleSlot = setopstate->ps.ps_ResultTupleSlot;

	/*
	 * We loop retrieving groups until we find one we should return
	 */
	while (!setopstate->setop_done)
	{
		MinimalTuple tuple;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Find the next entry in the hash table
		 */
		entry = ScanTupleHashTable(setopstate->hashtable, &setopstate->hashiter);

		if (entry == NULL)
		{
			/* No more entries in hashtable. But do we have spill files to process?  */
			if (setopstate->spillset)
			{
				if (setop_reload_batch(setopstate))
				{
					ResetTupleHashIterator(setopstate->hashtable, &setopstate->hashiter);
					continue;
				}
			}
			setopstate->setop_done = true;
			return NULL;
		}

		if (!entry->firstbatch)
		{
			/*
			 * re-spill this entry, because we might have spilled some earlier entries
			 * with this key already.
			 */
			setop_spill_entry(setopstate, entry, true);
			continue;
		}

		/*
		 * See if we should emit any copies of this tuple, and if so return
		 * the first copy.
		 */
		set_output_count(setopstate, (SetOpStatePerGroup) entry->additional);

		tuple = entry->firstTuple;

		free_pergroup(setopstate, entry->additional);
		setopstate->hashtable->usedMem -= sizeof(SetOpStatePerGroupData);
		tuplehash_delete_elem(setopstate->hashtable->hashtab, entry, entry->hash);
		setopstate->hashtable->usedMem -= GetMemoryChunkSpace(tuple);

		if (setopstate->numOutput > 0)
		{
			setopstate->numOutput--;
			return ExecStoreMinimalTuple(tuple,
										 resultTupleSlot,
										 true);
		}
		else
		{
			pfree(tuple);
		}
	}

	/* No more groups */
	ExecClearTuple(resultTupleSlot);
	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitSetOp
 *
 *		This initializes the setop node state structures and
 *		the node's subplan.
 * ----------------------------------------------------------------
 */
SetOpState *
ExecInitSetOp(SetOp *node, EState *estate, int eflags)
{
	SetOpState *setopstate;
	TupleDesc	outerDesc;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	setopstate = makeNode(SetOpState);
	setopstate->ps.plan = (Plan *) node;
	setopstate->ps.state = estate;
	setopstate->ps.ExecProcNode = ExecSetOp;

	setopstate->eqfuncoids = NULL;
	setopstate->hashfunctions = NULL;
	setopstate->setop_done = false;
	setopstate->numOutput = 0;
	setopstate->pergroup = NULL;
	setopstate->grp_firstTuple = NULL;
	setopstate->hashtable = NULL;
	setopstate->tableContext = NULL;

	/*
	 * create expression context
	 */
	ExecAssignExprContext(estate, &setopstate->ps);

	/*
	 * If hashing, we also need a longer-lived context to store the hash
	 * table.  The table can't just be kept in the per-query context because
	 * we want to be able to throw it away in ExecReScanSetOp.
	 */
	if (node->strategy == SETOP_HASHED)
		setopstate->tableContext =
			AllocSetContextCreate(CurrentMemoryContext,
								  "SetOp hash table",
								  ALLOCSET_DEFAULT_SIZES);

	/*
	 * initialize child nodes
	 *
	 * If we are hashing then the child plan does not need to handle REWIND
	 * efficiently; see ExecReScanSetOp.
	 */
	if (node->strategy == SETOP_HASHED)
		eflags &= ~EXEC_FLAG_REWIND;
	outerPlanState(setopstate) = ExecInitNode(outerPlan(node), estate, eflags);
	outerDesc = ExecGetResultType(outerPlanState(setopstate));

	/*
	 * Initialize result slot and type. Setop nodes do no projections, so
	 * initialize projection info for this node appropriately.
	 */
	ExecInitResultTupleSlotTL(estate, &setopstate->ps);
	setopstate->ps.ps_ProjInfo = NULL;

	/*
	 * Precompute fmgr lookup data for inner loop. We need both equality and
	 * hashing functions to do it by hashing, but only equality if not
	 * hashing.
	 */
	if (node->strategy == SETOP_HASHED)
		execTuplesHashPrepare(node->numCols,
							  node->dupOperators,
							  &setopstate->eqfuncoids,
							  &setopstate->hashfunctions);
	else
		setopstate->eqfunction =
			execTuplesMatchPrepare(outerDesc,
								   node->numCols,
								   node->dupColIdx,
								   node->dupOperators,
								   &setopstate->ps);

	if (node->strategy == SETOP_HASHED)
	{
		build_hash_table(setopstate);
		setopstate->table_filled = false;
	}
	else
	{
		setopstate->pergroup =
			(SetOpStatePerGroup) palloc0(sizeof(SetOpStatePerGroupData));
	}

	return setopstate;
}

/* ----------------------------------------------------------------
 *		ExecEndSetOp
 *
 *		This shuts down the subplan and frees resources allocated
 *		to this node.
 * ----------------------------------------------------------------
 */
void
ExecEndSetOp(SetOpState *node)
{
	/* clean up tuple table */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	if (node->spillset)
		CloseHashSpillSet(node->spillset);

	/* free subsidiary stuff including hashtable */
	if (node->tableContext)
		MemoryContextDelete(node->tableContext);
	ExecFreeExprContext(&node->ps);

	ExecEndNode(outerPlanState(node));
}


void
ExecReScanSetOp(SetOpState *node)
{
	ExecClearTuple(node->ps.ps_ResultTupleSlot);
	node->setop_done = false;
	node->numOutput = 0;

	if (((SetOp *) node->ps.plan)->strategy == SETOP_HASHED)
	{
		/*
		 * In the hashed case, if we haven't yet built the hash table then we
		 * can just return; nothing done yet, so nothing to undo. If subnode's
		 * chgParam is not NULL then it will be re-scanned by ExecProcNode,
		 * else no reason to re-scan it at all.
		 */
		if (!node->table_filled)
			return;

		/*
		 * If we do have the hash table and the subplan does not have any
		 * parameter changes, then we can just rescan the existing hash table;
		 * no need to build it again.
		 */
		if (node->ps.lefttree->chgParam == NULL)
		{
			ResetTupleHashIterator(node->hashtable, &node->hashiter);
			return;
		}
	}

	/* Release first tuple of group, if we have made a copy */
	if (node->grp_firstTuple != NULL)
	{
		heap_freetuple(node->grp_firstTuple);
		node->grp_firstTuple = NULL;
	}

	/* Release any hashtable storage */
	if (node->tableContext)
		MemoryContextResetAndDeleteChildren(node->tableContext);

	/* And rebuild empty hashtable if needed */
	if (((SetOp *) node->ps.plan)->strategy == SETOP_HASHED)
	{
		build_hash_table(node);
		node->table_filled = false;
	}

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (node->ps.lefttree->chgParam == NULL)
		ExecReScan(node->ps.lefttree);
}
