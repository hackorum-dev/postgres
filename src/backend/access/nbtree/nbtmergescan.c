/*-------------------------------------------------------------------------
 *
 * nbtmergescan.c
 *	  B-Tree merge scan for efficient evaluation of IN-list queries
 *
 * This module implements a K-way merge scan for B-tree indexes, optimized
 * for queries of the form:
 *   WHERE prefix IN (v1, v2, ..., vK) AND suffix >= b ORDER BY suffix LIMIT N
 *
 * The algorithm maintains a min-heap of cursors, one per prefix value.
 * Each cursor tracks its position within the index for that prefix.
 * Tuples are returned in suffix order by repeatedly extracting the
 * minimum from the heap.
 *
 * Target behavior: Access at most N + K - 1 index tuples for LIMIT N.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtmergescan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/nbtree.h"
#include "access/relscan.h"
#include "lib/pairingheap.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* Forward declarations of static functions */
static int	bt_merge_heap_cmp(const pairingheap_node *a,
							  const pairingheap_node *b,
							  void *arg);
static bool bt_merge_cursor_init(BTMergeScanState *state,
								 IndexScanDesc scan,
								 BTMergeCursor *cursor);
static bool bt_merge_cursor_advance(BTMergeScanState *state,
									IndexScanDesc scan,
									BTMergeCursor *cursor);
static IndexTuple bt_merge_get_index_tuple(BTMergeCursor *cursor);


/*
 * bt_merge_get_index_tuple
 *	  Get the current index tuple from a cursor.
 *
 * Returns the IndexTuple pointer from cursor->tuples, or NULL if exhausted.
 */
static IndexTuple
bt_merge_get_index_tuple(BTMergeCursor *cursor)
{
	BTScanPosItem *currItem;

	if (cursor->exhausted || cursor->tuples == NULL)
		return NULL;

	currItem = &cursor->pos.items[cursor->pos.itemIndex];
	return (IndexTuple) (cursor->tuples + currItem->tupleOffset);
}

/*
 * bt_merge_heap_cmp
 *	  Compare two cursors by their current sort key (all suffix columns).
 *
 * Compares all suffix columns in order. When all suffix columns are equal,
 * uses cursor_id as tiebreaker for deterministic ordering (preserves
 * original prefix array order).
 *
 * returns
 *    -1 if a comes before b
 *     1 if b comes before a
 *     0 if a and b are equal
 */
static int
bt_merge_heap_cmp(const pairingheap_node *a,
				  const pairingheap_node *b,
				  void *arg)
{
	BTMergeScanState *state = (BTMergeScanState *) arg;
	BTMergeCursor *cursor_a = pairingheap_container(BTMergeCursor, ph_node,
													(pairingheap_node *) a);
	BTMergeCursor *cursor_b = pairingheap_container(BTMergeCursor, ph_node,
													(pairingheap_node *) b);
	IndexTuple	itup_a;
	IndexTuple	itup_b;
	int32		cmp = 0;
	int			col;

	/* Get the index tuples from each cursor */
	itup_a = bt_merge_get_index_tuple(cursor_a);
	itup_b = bt_merge_get_index_tuple(cursor_b);

	/* Handle exhausted cursors */
	if (itup_a == NULL && itup_b == NULL)
		return cursor_b->cursor_id - cursor_a->cursor_id;
	if (itup_a == NULL)
		return -1;				/* a is exhausted, comes after b */
	if (itup_b == NULL)
		return 1;				/* b is exhausted, comes after a */

	/* Compare all suffix columns in order */
	for (col = 0; col < state->index_rel->rd_index->indnkeyatts - state->num_prefix_cols && cmp == 0; col++)
	{
		int			attno = state->num_prefix_cols + col + 1;
		int16		indoption = state->index_rel->rd_indoption[attno - 1];
		bool		null_a,
					null_b;
		Datum		key_a,
					key_b;

		key_a = index_getattr(itup_a, attno, state->index_tupdesc, &null_a);
		key_b = index_getattr(itup_b, attno, state->index_tupdesc, &null_b);

		/* Handle NULLs - return directly with all factors multiplied */
		if (null_a || null_b)
		{
			if (null_a && null_b)
				continue;		/* Both NULL, try next column */

			return (null_a ? -1 : 1)
				 * ((indoption & INDOPTION_NULLS_FIRST) ? -1 : 1)
				 * (state->direction == BackwardScanDirection ? -1 : 1);
		}

		/* Compare using index's comparison function and collation */
		cmp = DatumGetInt32(FunctionCall2Coll(index_getprocinfo(state->index_rel, attno, BTORDER_PROC),
											  TupleDescAttr(state->index_tupdesc, attno - 1)->attcollation,
											  key_a, key_b));

		/* For DESC columns, invert to match physical index order */
		if ((indoption & INDOPTION_DESC))
			cmp = -cmp;
	}

	/* For backward scan, invert the suffix comparison */
	if (state->direction == BackwardScanDirection)
		cmp = -cmp;

	/* Use cursor_id as tiebreaker (always ascending for determinism) */
	if (cmp == 0)
		cmp = cursor_a->cursor_id - cursor_b->cursor_id;

	/* Negate for min-heap behavior */
	return -cmp;
}


/*
 * bt_merge_init
 *	  Initialize a merge scan state.
 *
 * Creates the merge state with one cursor per prefix combination.
 * The cursors will be positioned at their first matching tuples
 * when bt_merge_getnext is first called.
 *
 * Prefix columns are assumed to be 1..num_prefix_cols.
 * Suffix columns are (num_prefix_cols+1)..indnkeyatts.
 * Comparison functions are looked up from the index relation.
 */
BTMergeScanState *
bt_merge_init(IndexScanDesc scan,
			  Datum **prefix_tuples,
			  bool **prefix_nulls,
			  int num_cursors,
			  int num_prefix_cols)
{
	BTMergeScanState *state;
	Relation	rel = scan->indexRelation;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	MemoryContext merge_context;
	MemoryContext old_context;
	int			i;
	int			j;

	/* Check there are suffix columns to order by */
	if (rel->rd_index->indnkeyatts <= num_prefix_cols)
		return NULL;

	/* Create memory context for merge scan allocations */
	merge_context = AllocSetContextCreate(CurrentMemoryContext,
										  "BTMergeScan",
										  ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(merge_context);

	/* Allocate main state structure */
	state = palloc0(sizeof(BTMergeScanState));
	state->merge_context = merge_context;
	state->num_cursors = num_cursors;
	state->active_cursors = 0;
	state->num_prefix_cols = num_prefix_cols;
	state->direction = ForwardScanDirection;
	state->initialized = false;
	state->tuples_accessed = 0;
	state->index_tupdesc = tupdesc;

	/* Store reference to index relation (for cmp funcs, collations, indoption) */
	state->index_rel = rel;

	/* Allocate cursor array */
	state->cursors = palloc0(num_cursors * sizeof(BTMergeCursor));

	/* Initialize cursor metadata (not positioned yet) */
	for (i = 0; i < num_cursors; i++)
	{
		BTMergeCursor *cursor = &state->cursors[i];
		bool		has_null = false;

		cursor->cursor_id = i;

		/* Check if any prefix value is NULL */
		for (j = 0; j < num_prefix_cols; j++)
		{
			if (prefix_nulls[i][j])
			{
				has_null = true;
				break;
			}
		}

		/* Skip cursors with NULL prefixes - they would match nothing */
		if (has_null)
		{
			cursor->prefix_values = NULL;
			cursor->exhausted = true;
			cursor->tuples = NULL;
			BTScanPosInvalidate(cursor->pos);
			continue;
		}

		/* Copy prefix values for this cursor */
		cursor->prefix_values = palloc(num_prefix_cols * sizeof(Datum));
		for (j = 0; j < num_prefix_cols; j++)
		{
			cursor->prefix_values[j] = datumCopy(prefix_tuples[i][j], true, sizeof(Datum));
		}
		cursor->exhausted = false;
		BTScanPosInvalidate(cursor->pos);
		/* Allocate tuple workspace for suffix key extraction */
		cursor->tuples = palloc(BLCKSZ);
	}

	/* Initialize the merge heap */
	state->merge_heap = pairingheap_allocate(bt_merge_heap_cmp, state);

	MemoryContextSwitchTo(old_context);

	return state;
}


/*
 * bt_merge_getnext
 *	  Get the next tuple from the merge scan.
 *
 * Returns true if a tuple was found, false if scan is exhausted.
 * The tuple's TID is stored in scan->xs_heaptid.
 */
bool
bt_merge_getnext(IndexScanDesc scan, ScanDirection dir)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	BTMergeScanState *state = so->mergeState;
	BTMergeCursor *cursor;
	pairingheap_node *node;
	int			i;

	if (state == NULL)
		return false;

	/* Initialize cursors on first call */
	if (!state->initialized)
	{
		state->initialized = true;
		state->direction = dir;

		for (i = 0; i < state->num_cursors; i++)
		{
			BTMergeCursor *c = &state->cursors[i];

			if (!c->exhausted && bt_merge_cursor_init(state, scan, c))
			{
				/* Cursor has at least one tuple, add to heap */
				pairingheap_add(state->merge_heap, &c->ph_node);
				state->active_cursors++;
			}
		}

		/*
		 * Track internal tuple reads for stats. We read active_cursors tuples
		 * during initialization. One of these will be returned first and
		 * counted by index_getnext_tid, so we count (active_cursors - 1) here.
		 */
		if (state->active_cursors > 1)
			pgstat_count_index_tuples(scan->indexRelation,
									  state->active_cursors - 1);
	}

	/* Get the cursor with the smallest suffix value */
	if (pairingheap_is_empty(state->merge_heap))
		return false;

	node = pairingheap_remove_first(state->merge_heap);
	cursor = pairingheap_container(BTMergeCursor, ph_node, node);

	/* Set up the heap TID and index tuple from the current cursor position */
	Assert(BTScanPosIsValid(cursor->pos));
	{
		BTScanPosItem *currItem = &cursor->pos.items[cursor->pos.itemIndex];
		scan->xs_heaptid = currItem->heapTid;
		/* For index-only scans, set the index tuple pointer */
		if (cursor->tuples)
			scan->xs_itup = (IndexTuple) (cursor->tuples + currItem->tupleOffset);
	}

	/* Advance cursor to next tuple */
	if (bt_merge_cursor_advance(state, scan, cursor))
	{
		/* Cursor still has tuples, re-add to heap */
		pairingheap_add(state->merge_heap, &cursor->ph_node);
	}
	else
	{
		/* Cursor exhausted */
		state->active_cursors--;
	}

	return true;
}


/*
 * bt_merge_end
 *	  Clean up merge scan state.
 */
void
bt_merge_end(BTMergeScanState *state)
{
	int			i;

	if (state == NULL)
		return;

	/* Release any buffer pins held by cursors */
	for (i = 0; i < state->num_cursors; i++)
	{
		BTMergeCursor *cursor = &state->cursors[i];

		if (BTScanPosIsValid(cursor->pos) && BufferIsValid(cursor->pos.buf))
		{
			ReleaseBuffer(cursor->pos.buf);
			cursor->pos.buf = InvalidBuffer;
		}
	}

	/* Free the memory context, which frees all allocations */
	MemoryContextDelete(state->merge_context);
}


/*
 * bt_merge_cursor_init
 *	  Initialize a cursor and position it at the first matching tuple.
 *
 * Returns true if the cursor found at least one matching tuple.
 */
static bool
bt_merge_cursor_init(BTMergeScanState *state,
					 IndexScanDesc scan,
					 BTMergeCursor *cursor)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	bool		found;
	bool		save_want_itup;
	int			col;

	/*
	 * Modify the scan keys to use this cursor's prefix values.
	 * We modify scan->keyData (original keys) because _bt_first calls
	 * _bt_preprocess_keys which re-processes scan->keyData into so->keyData.
	 * Prefix columns are 1..num_prefix_cols.
	 */
	for (col = 0; col < state->num_prefix_cols; col++)
	{
		int			attno = col + 1;	/* 1-based attribute number */

		for (int i = 0; i < scan->numberOfKeys; i++)
		{
			if (scan->keyData[i].sk_attno == attno &&
				scan->keyData[i].sk_strategy == BTEqualStrategyNumber)
			{
				scan->keyData[i].sk_argument = cursor->prefix_values[col];
				scan->keyData[i].sk_flags &= ~(SK_SEARCHARRAY);
				break;
			}
		}
	}

	/* Force key re-preprocessing for this cursor's prefix values */
	so->numberOfKeys = 0;

	/* Invalidate current position to force _bt_first */
	BTScanPosInvalidate(so->currPos);

	/*
	 * Disable array key handling for this cursor's scan.
	 * We need to clear both numArrayKeys and needPrimScan to avoid
	 * assertions in _bt_readfirstpage that expect array keys when
	 * needPrimScan is set.
	 */
	so->numArrayKeys = 0;
	so->needPrimScan = false;

	/*
	 * Force tuple data to be copied for suffix key extraction.
	 * This is needed even for regular (non-index-only) scans because
	 * the merge comparison function needs access to the suffix column.
	 */
	save_want_itup = scan->xs_want_itup;
	scan->xs_want_itup = true;

	/* Position at first matching tuple */
	found = _bt_first(scan, state->direction);

	if (found)
	{
		/* Copy position to cursor */
		memcpy(&cursor->pos, &so->currPos, sizeof(BTScanPosData));

		/*
		 * Copy the tuple data for suffix key extraction during heap comparison.
		 * The tuple workspace contains copies of index tuples referenced
		 * by items in currPos.
		 */
		if (so->currTuples && so->currPos.nextTupleOffset > 0)
		{
			memcpy(cursor->tuples, so->currTuples, so->currPos.nextTupleOffset);
		}

		cursor->exhausted = false;
		state->tuples_accessed++;

		/* Invalidate main scan position */
		BTScanPosInvalidate(so->currPos);
	}
	else
	{
		cursor->exhausted = true;
	}

	/* Restore original setting */
	scan->xs_want_itup = save_want_itup;

	return found;
}


/*
 * bt_merge_cursor_advance
 *	  Advance a cursor to its next tuple.
 *
 * Returns true if the cursor now points to a valid tuple, false if exhausted.
 */
static bool
bt_merge_cursor_advance(BTMergeScanState *state,
						IndexScanDesc scan,
						BTMergeCursor *cursor)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	bool		found = false;

	if (cursor->exhausted)
		return false;

	/* Try to move to next tuple within current page's items array */
	if (state->direction == ForwardScanDirection)
	{
		if (cursor->pos.itemIndex < cursor->pos.lastItem)
		{
			cursor->pos.itemIndex++;
			found = true;
		}
	}
	else
	{
		if (cursor->pos.itemIndex > cursor->pos.firstItem)
		{
			cursor->pos.itemIndex--;
			found = true;
		}
	}

	if (!found)
	{
		/*
		 * Current page exhausted. Use _bt_next to get the next page.
		 * We swap our cursor's position into the scan's currPos,
		 * call _bt_next, then swap back.
		 */
		BTScanPosData save_pos;
		bool		save_want_itup;

		memcpy(&save_pos, &so->currPos, sizeof(BTScanPosData));
		memcpy(&so->currPos, &cursor->pos, sizeof(BTScanPosData));

		/* Force tuple data to be copied for suffix key extraction */
		save_want_itup = scan->xs_want_itup;
		scan->xs_want_itup = true;

		found = _bt_next(scan, state->direction);

		if (found)
		{
			memcpy(&cursor->pos, &so->currPos, sizeof(BTScanPosData));

			/*
			 * Copy the new page's tuple data for suffix key extraction.
			 */
			if (so->currTuples && so->currPos.nextTupleOffset > 0)
			{
				memcpy(cursor->tuples, so->currTuples, so->currPos.nextTupleOffset);
			}
		}

		/* Restore original setting */
		scan->xs_want_itup = save_want_itup;

		memcpy(&so->currPos, &save_pos, sizeof(BTScanPosData));
	}

	if (found)
	{
		state->tuples_accessed++;
	}
	else
	{
		cursor->exhausted = true;
	}

	return found;
}
