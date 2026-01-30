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

#include "access/nbtree.h"
#include "access/relscan.h"
#include "lib/pairingheap.h"
#include "miscadmin.h"
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
								 BTMergeCursor *cursor,
								 Datum prefix_value,
								 bool prefix_isnull);
static bool bt_merge_cursor_advance(BTMergeScanState *state,
									IndexScanDesc scan,
									BTMergeCursor *cursor);
static Datum bt_merge_extract_sortkey(BTMergeScanState *state,
									  IndexScanDesc scan,
									  BTMergeCursor *cursor,
									  bool *isnull);


/*
 * bt_merge_heap_cmp
 *	  Compare two cursors by their current sort key (suffix value).
 *
 * When sort keys are equal, uses prefix value as tiebreaker for
 * deterministic ordering (ORDER BY suffix, prefix).
 *
 * Returns positive if a > b (pairingheap is a max-heap, we want min-heap
 * behavior so we invert the comparison).
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
	Datum		key_a = cursor_a->sort_key;
	Datum		key_b = cursor_b->sort_key;
	bool		null_a = cursor_a->sort_key_isnull;
	bool		null_b = cursor_b->sort_key_isnull;
	int32		cmp;

	/* Handle NULLs - NULLs sort last (NULLS LAST default for ASC) */
	if (null_a && null_b)
		return 0;
	if (null_a)
		return -1;				/* a is NULL, comes after b */
	if (null_b)
		return 1;				/* b is NULL, comes after a */

	/* Compare using the suffix column's comparison function */
	cmp = DatumGetInt32(FunctionCall2Coll(&state->suffix_cmp,
										  state->suffix_collation,
										  key_a, key_b));

	/*
	 * Use prefix value as tiebreaker for deterministic ordering.
	 * This ensures ORDER BY suffix, prefix behavior.
	 */
	if (cmp == 0)
	{
		/* Compare prefix values (assumes pass-by-value int4 for now) */
		int32		prefix_a = DatumGetInt32(cursor_a->prefix_value);
		int32		prefix_b = DatumGetInt32(cursor_b->prefix_value);

		if (prefix_a < prefix_b)
			cmp = -1;
		else if (prefix_a > prefix_b)
			cmp = 1;
	}

	/* Negate for min-heap behavior */
	return -cmp;
}


/*
 * bt_merge_init
 *	  Initialize a merge scan state.
 *
 * Creates the merge state with one cursor per prefix value.
 * The cursors will be positioned at their first matching tuples
 * when bt_merge_getnext is first called.
 */
BTMergeScanState *
bt_merge_init(IndexScanDesc scan,
			  Datum *prefix_values,
			  bool *prefix_nulls,
			  int num_prefixes,
			  int prefix_attno,
			  int suffix_attno,
			  Oid suffix_cmp_oid,
			  Oid suffix_collation)
{
	BTMergeScanState *state;
	MemoryContext merge_context;
	MemoryContext old_context;
	int			i;

	/* Create memory context for merge scan allocations */
	merge_context = AllocSetContextCreate(CurrentMemoryContext,
										  "BTMergeScan",
										  ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(merge_context);

	/* Allocate main state structure */
	state = palloc0(sizeof(BTMergeScanState));
	state->merge_context = merge_context;
	state->num_cursors = num_prefixes;
	state->active_cursors = 0;
	state->prefix_attno = prefix_attno;
	state->suffix_attno = suffix_attno;
	state->suffix_collation = suffix_collation;
	state->direction = ForwardScanDirection;
	state->initialized = false;
	state->tuples_accessed = 0;

	/* Set up suffix comparison function */
	fmgr_info(suffix_cmp_oid, &state->suffix_cmp);

	/* Allocate cursor array */
	state->cursors = palloc0(num_prefixes * sizeof(BTMergeCursor));

	/* Initialize cursor metadata (not positioned yet) */
	for (i = 0; i < num_prefixes; i++)
	{
		BTMergeCursor *cursor = &state->cursors[i];

		cursor->cursor_id = i;
		cursor->prefix_value = datumCopy(prefix_values[i], true, sizeof(Datum));
		cursor->prefix_isnull = prefix_nulls[i];
		cursor->exhausted = prefix_nulls[i];	/* NULL prefix = exhausted */
		cursor->sort_key_isnull = true;
		BTScanPosInvalidate(cursor->pos);
		cursor->tuples = NULL;
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

			if (!c->exhausted &&
				bt_merge_cursor_init(state, scan, c,
									 c->prefix_value, c->prefix_isnull))
			{
				/* Cursor has at least one tuple, add to heap */
				pairingheap_add(state->merge_heap, &c->ph_node);
				state->active_cursors++;
			}
		}
	}

	/* Get the cursor with the smallest suffix value */
	if (pairingheap_is_empty(state->merge_heap))
		return false;

	node = pairingheap_remove_first(state->merge_heap);
	cursor = pairingheap_container(BTMergeCursor, ph_node, node);

	/* Set up the heap TID from the current cursor position */
	Assert(BTScanPosIsValid(cursor->pos));
	scan->xs_heaptid = cursor->pos.items[cursor->pos.itemIndex].heapTid;

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
	if (state == NULL)
		return;

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
					 BTMergeCursor *cursor,
					 Datum prefix_value,
					 bool prefix_isnull)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	bool		found;

	if (prefix_isnull)
	{
		cursor->exhausted = true;
		return false;
	}

	/*
	 * Modify the scan key to use this cursor's prefix value.
	 * We reuse the scan's existing key infrastructure.
	 */
	for (int i = 0; i < so->numberOfKeys; i++)
	{
		if (so->keyData[i].sk_attno == state->prefix_attno)
		{
			so->keyData[i].sk_argument = prefix_value;
			so->keyData[i].sk_flags &= ~(SK_SEARCHARRAY);
			break;
		}
	}

	/* Invalidate current position to force _bt_first */
	BTScanPosInvalidate(so->currPos);

	/* Disable array key handling for this cursor's scan */
	so->numArrayKeys = 0;

	/* Position at first matching tuple */
	found = _bt_first(scan, state->direction);

	if (found)
	{
		/* Copy position to cursor */
		memcpy(&cursor->pos, &so->currPos, sizeof(BTScanPosData));

		/* Extract the sort key for heap ordering */
		cursor->sort_key = bt_merge_extract_sortkey(state, scan, cursor,
													&cursor->sort_key_isnull);
		cursor->exhausted = false;

		/* Count this as a tuple access */
		state->tuples_accessed++;

		/* Invalidate main scan position */
		BTScanPosInvalidate(so->currPos);
	}
	else
	{
		cursor->exhausted = true;
	}

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

		memcpy(&save_pos, &so->currPos, sizeof(BTScanPosData));
		memcpy(&so->currPos, &cursor->pos, sizeof(BTScanPosData));

		found = _bt_next(scan, state->direction);

		if (found)
			memcpy(&cursor->pos, &so->currPos, sizeof(BTScanPosData));

		memcpy(&so->currPos, &save_pos, sizeof(BTScanPosData));
	}

	if (found)
	{
		/* Extract new sort key */
		cursor->sort_key = bt_merge_extract_sortkey(state, scan, cursor,
													&cursor->sort_key_isnull);
		state->tuples_accessed++;
	}
	else
	{
		cursor->exhausted = true;
	}

	return found;
}


/*
 * bt_merge_extract_sortkey
 *	  Extract the sort key (suffix column value) from the current tuple.
 */
static Datum
bt_merge_extract_sortkey(BTMergeScanState *state,
						 IndexScanDesc scan,
						 BTMergeCursor *cursor,
						 bool *isnull)
{
	Relation	rel = scan->indexRelation;
	Buffer		buf;
	Page		page;
	OffsetNumber offnum;
	ItemId		itemid;
	IndexTuple	itup;
	TupleDesc	tupdesc;
	Datum		result;

	if (cursor->pos.currPage == InvalidBlockNumber)
	{
		*isnull = true;
		return (Datum) 0;
	}

	/* Read the page */
	buf = ReadBuffer(rel, cursor->pos.currPage);
	LockBuffer(buf, BT_READ);
	page = BufferGetPage(buf);

	offnum = cursor->pos.items[cursor->pos.itemIndex].indexOffset;
	itemid = PageGetItemId(page, offnum);
	itup = (IndexTuple) PageGetItem(page, itemid);
	tupdesc = RelationGetDescr(rel);

	/* Extract the suffix column value */
	result = index_getattr(itup, state->suffix_attno, tupdesc, isnull);

	/* Copy pass-by-reference values before releasing buffer */
	if (!*isnull)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, state->suffix_attno - 1);

		if (!attr->attbyval)
			result = datumCopy(result, attr->attbyval, attr->attlen);
	}

	UnlockReleaseBuffer(buf);

	return result;
}
