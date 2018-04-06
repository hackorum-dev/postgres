/*-------------------------------------------------------------------------
 *
 * vacuumtidmap.c
 *	  Data structure to hold TIDs of dead tuples during vacuum.
 *
 * Vacuum Tid Map is used to hold the TIDs of dead tuples during VACUUM.
 * The data structure is a fairly straightforward B-tree, but tailored for
 * storing TIDs.
 *
 * Operations we need to support:
 *
 * - Adding new TIDs. TIDs are only added in increasing TID order.  Thanks
 *   to that, we can fill each internal node fully, and never need to split
 *   existing nodes.
 *
 * - Random lookups by TID.  This is done heavily while scanning indexes.
 *
 * - Scan all TIDs in order.  In the 2nd phase of vacuum.  To make that
 *   simpler, we store a 'next' pointer on each leaf node.
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/vacuumtidmap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "commands/vacuum.h"
#include "utils/memutils.h"

/*
 * Node structures, for the in-memory B-tree.
 *
 * Each leaf node stores a number of TIDs, in a sorted array.  There is also
 * a pointer to the next leaf node, so that the leaf nodes can be easily
 * walked from beginning to end.
 *
 * An internal node holds a number of downlink pointers, to leaf nodes, or
 * nodes to internal nodes on lower level.  For each downlink, the TID
 * corresponding lower level node is stored.

 * Note that because these form just an in-memory data structure, there is no
 * need for the nodes to be of the same size.
 */

#define MAX_LEAF_NODE_ITEMS			2048
#define MAX_INTERNAL_NODE_ITEMS		64

/* common structure of both leaf and internal nodes. */
typedef struct
{
	int			level;
} VacuumTidMapNode;

typedef struct VacuumTidMapLeafNode VacuumTidMapLeafNode;

struct VacuumTidMapLeafNode
{
	int			level;			/* always 0 on leaf nodes. */
	VacuumTidMapLeafNode *next;
	int			num_items;
	ItemPointerData itemptrs[MAX_LEAF_NODE_ITEMS];
};

typedef struct
{
	int			level;			/* always >= 1 on internal nodes */
	int			num_items;
	ItemPointerData itemptrs[MAX_INTERNAL_NODE_ITEMS];
	VacuumTidMapNode *downlinks[MAX_INTERNAL_NODE_ITEMS];
} VacuumTidMapInternalNode;

/* Maximum height of the tree. 10 should be plenty. */
#define MAX_LEVELS		10

struct VacuumTidMap
{
	int			num_levels;
	uint64		num_entries;	/* total # of tids in the tree */

	/* Memory context, to hold all extra nodes */
	MemoryContext context;

	/* Memory tracking */
	uint64		mem_max;
	uint64		mem_used;

	/*
	 * 'root' points to the root of the tree (or the only leaf node, if
	 * num_levels == 1).  'first_leaf' points to the leftmost leaf page.
	 */
	VacuumTidMapNode *root;
	VacuumTidMapLeafNode *first_leaf;

	/*
	 * Pointer to the rightmost leaf page, and its parent, grandparent etc.
	 * all the way up to the root.
	 */
	VacuumTidMapLeafNode *last_leaf;
	VacuumTidMapInternalNode *last_parents[MAX_LEVELS];

	/* Iterator support */
	OffsetNumber *iter_offsets;
	VacuumTidMapLeafNode *iter_node;
	int			iter_itemno;
};

static inline int vac_itemptr_binsrch(BlockNumber refblk, OffsetNumber refoff, ItemPointer arr, int arr_elems);
static void update_upper(VacuumTidMap * dt, int level, void *new_node, ItemPointer new_node_itemptr);

/*
 * Create a new, initially empty, tidmap.
 *
 * 'vac_work_mem' is the max amount of memory to be used.  The tidmap doesn't
 * actually limit the amount of memory used in any way, but it affects when
 * VacuumTidMapIsFull() says that the tidmap is full.
 */
VacuumTidMap *
CreateVacuumTidMap(int vac_work_mem)
{
	VacuumTidMap *dt;

	/*
	 * Allocate the tid map struct, and the first leaf node in the current
	 * memory context.  Any additional nodes are allocated in a separate
	 * context, so that they can be freed easily in VacuumTidMapReset().
	 */
	dt = (VacuumTidMap *) palloc(sizeof(VacuumTidMap));

	dt->context = AllocSetContextCreate(CurrentMemoryContext,
										"Vacuum TID map",
										ALLOCSET_DEFAULT_SIZES);
	dt->num_entries = 0;

	dt->first_leaf = (VacuumTidMapLeafNode *)
		palloc(sizeof(VacuumTidMapLeafNode));
	dt->first_leaf->level = 0;
	dt->first_leaf->next = NULL;
	dt->first_leaf->num_items = 0;

	dt->last_leaf = dt->first_leaf;
	dt->root = (VacuumTidMapNode *) dt->first_leaf;
	dt->num_levels = 1;

	dt->mem_max = ((uint64) vac_work_mem) * 1024;
	dt->mem_used = sizeof(VacuumTidMap) + sizeof(VacuumTidMapLeafNode);

	dt->iter_offsets = NULL;	/* no iteration in progress */

	return dt;
}

/*
 * Clear all items from the tid map.
 */
void
VacuumTidMapReset(VacuumTidMap *dt)
{
	dt->num_entries = 0;

	dt->first_leaf->next = NULL;
	dt->first_leaf->num_items = 0;
	dt->last_leaf = dt->first_leaf;
	dt->root = (VacuumTidMapNode *) dt->first_leaf;
	dt->num_levels = 1;
	MemoryContextReset(dt->context);

	dt->mem_used = sizeof(VacuumTidMap) + sizeof(VacuumTidMapLeafNode);
	dt->iter_offsets = NULL;
}

bool
VacuumTidMapIsEmpty(VacuumTidMap *dt)
{
	return (dt->num_entries == 0);
}

/*
 * Returns 'true', if inserting another heap page's worth of dead tuples would
 * overrun the allocated memory.
 */
bool
VacuumTidMapIsFull(VacuumTidMap *dt)
{
	/* Can we fit a whole heap page's worth of items on this leaf node? */
	if (MAX_LEAF_NODE_ITEMS - dt->last_leaf->num_items >= MaxHeapTuplesPerPage)
		return false;

	/*
	 * Do we have space to allocate another leaf node?
	 *
	 * XXX: This doesn't take into account the possibility that allocating a
	 * new leaf page also requires allocating new internal pages, possibly all
	 * the way up to the root.  So we might still overshoot if that happens.
	 */
	if (dt->mem_max - dt->mem_used > sizeof(VacuumTidMapLeafNode))
		return false;

	return true;
}

uint64
VacuumTidMapGetNumTuples(VacuumTidMap *dt)
{
	return dt->num_entries;
}

/*
 * Begin in-order scan through all the TIDs.
 */
void
VacuumTidMapBeginIterate(VacuumTidMap *dt)
{
	if (dt->iter_offsets)
		elog(ERROR, "iteration on vacuum tid map is already in progress");

	dt->iter_offsets = MemoryContextAlloc(dt->context,
										  MaxHeapTuplesPerPage * sizeof(OffsetNumber));
	dt->iter_node = dt->first_leaf;
	dt->iter_itemno = 0;
}

/*
 * Returns next batch of tuples.
 *
 * VacuumTidMapBeginIterate() must be called first.  VacuumTidMapNext() returns
 * TIDs one block number at a time, such that each call returns all the TIDs
 * with the same block number.
 *
 * If there are any more entries, returns true, and 'blkno', 'ntuples' and
 * 'offsets' are filled with the next batch of TIDs.
 */
bool
VacuumTidMapNext(VacuumTidMap *dt, BlockNumber *blkno, int *ntuples, OffsetNumber **offsets)
{
	int			curr_itemno;
	BlockNumber currblk;
	VacuumTidMapLeafNode *curr_node;
	int			num_offsets;

	if (!dt->iter_offsets)
		elog(ERROR, "vacuum tid map iteration is not in progress");

	if (!dt->iter_node)
	{
		/* No more TIDs.  End the iterator, and return false */
		pfree(dt->iter_offsets);
		dt->iter_offsets = NULL;
		return false;
	}

	Assert(dt->iter_node->level == 0);
	Assert(dt->iter_node->num_items > 0);

	/*
	 * There are more TIDs to return.
	 *
	 * Grab the block number from the next TID, and scan forward, collecting
	 * the offset numbers of all TIDs on the same block.
	 */
	curr_node = dt->iter_node;
	curr_itemno = dt->iter_itemno;

	currblk = ItemPointerGetBlockNumber(&curr_node->itemptrs[curr_itemno]);
	dt->iter_offsets[0] = ItemPointerGetOffsetNumber(&curr_node->itemptrs[curr_itemno]);
	num_offsets = 1;
	curr_itemno++;

	for (;;)
	{
		if (curr_itemno >= curr_node->num_items)
		{
			/* We have reached end of this node.  Step to the next one. */
			curr_node = curr_node->next;
			curr_itemno = 0;

			if (!curr_node)
			{
				/* Reached the very end. */
				break;
			}
		}

		if (ItemPointerGetBlockNumber(&curr_node->itemptrs[curr_itemno]) != currblk)
			break;

		dt->iter_offsets[num_offsets] =
			ItemPointerGetOffsetNumber(&curr_node->itemptrs[curr_itemno]);
		num_offsets++;
		curr_itemno++;
	}

	/* Remember where we stopped, for the next call */
	dt->iter_node = curr_node;
	dt->iter_itemno = curr_itemno;

	*blkno = currblk;
	*ntuples = num_offsets;
	*offsets = dt->iter_offsets;
	return true;
}

/*
 * Record a TID in the map.
 *
 * TIDs must be recorded in order.
 */
void
VacuumTidMapRecordTid(VacuumTidMap *dt, ItemPointer itemptr)
{
	VacuumTidMapLeafNode *last_leaf;

	/* The new TID should be larger than the last one recorded */
	Assert(ItemPointerIsValid(itemptr));
	Assert(dt->num_entries == 0 ||
		   ItemPointerCompare(itemptr, &dt->last_leaf->itemptrs[dt->last_leaf->num_items - 1]) > 0);

	/* Allocate a new leaf node if needed */
	if (dt->last_leaf->num_items == MAX_LEAF_NODE_ITEMS)
	{
		VacuumTidMapLeafNode *new_node;

		dt->mem_used += sizeof(VacuumTidMapLeafNode);
		new_node = (VacuumTidMapLeafNode *)
			MemoryContextAlloc(dt->context, sizeof(VacuumTidMapLeafNode));
		new_node->level = 0;
		new_node->next = NULL;
		new_node->num_items = 0;

		dt->last_leaf->next = new_node;
		dt->last_leaf = new_node;

		update_upper(dt, 1, new_node, itemptr);
	}
	last_leaf = dt->last_leaf;

	last_leaf->itemptrs[last_leaf->num_items] = *itemptr;
	last_leaf->num_items++;

	dt->num_entries++;
}

/*
 * Insert the downlink into parent node, after creating a new node.
 *
 * Recurses if the parent node is full, too.
 */
static void
update_upper(VacuumTidMap *dt, int level, void *new_node,
			 ItemPointer new_node_itemptr)
{
	VacuumTidMapInternalNode *parent;

	/* Append to the parent. */
	if (level >= dt->num_levels)
	{
		/* Create new root node. */
		ItemPointerData old_root_itemptr;

		/* MAX_LEVELS should be more than enough, so this shouldn't happen */
		if (dt->num_levels == MAX_LEVELS)
			elog(ERROR, "could not expand vacuum tid map, maximum number of levels reached");

		if (dt->num_levels == 1)
			old_root_itemptr = ((VacuumTidMapLeafNode *) dt->root)->itemptrs[0];
		else
			old_root_itemptr = ((VacuumTidMapInternalNode *) dt->root)->itemptrs[0];

		dt->mem_used += sizeof(VacuumTidMapInternalNode);
		parent = (VacuumTidMapInternalNode *)
			MemoryContextAlloc(dt->context, sizeof(VacuumTidMapInternalNode));
		parent->level = level;
		parent->itemptrs[0] = old_root_itemptr;
		parent->downlinks[0] = dt->root;
		parent->num_items = 1;

		dt->last_parents[level] = parent;
		dt->root = (VacuumTidMapNode *) parent;
		dt->num_levels++;
	}

	parent = dt->last_parents[level];

	if (parent->num_items < MAX_INTERNAL_NODE_ITEMS)
	{
		parent->itemptrs[parent->num_items] = *new_node_itemptr;
		parent->downlinks[parent->num_items] = new_node;
		parent->num_items++;
	}
	else
	{
		/* Doesn't fit on the parent.  Allocate new parent. */
		dt->mem_used += sizeof(VacuumTidMapInternalNode);
		parent = (VacuumTidMapInternalNode *) MemoryContextAlloc(dt->context,
																 sizeof(VacuumTidMapInternalNode));
		parent->level = level;
		parent->itemptrs[0] = *new_node_itemptr;
		parent->downlinks[0] = new_node;
		parent->num_items = 1;

		dt->last_parents[level] = parent;

		/* Link this new parent to its parent */
		update_upper(dt, level + 1, parent, new_node_itemptr);
	}
}

/*
 * Does the tid map contain the given TID?
 */
bool
VacuumTidMapContainsTid(VacuumTidMap *dt, ItemPointer itemptr)
{
	VacuumTidMapLeafNode *leaf_node;
	VacuumTidMapInternalNode *node;
	int			level = dt->num_levels - 1;
	int			itemno;
	BlockNumber refblk;
	OffsetNumber refoff;

	if (dt->num_entries == 0 || !ItemPointerIsValid(itemptr))
		return false;

	refblk = ItemPointerGetBlockNumberNoCheck(itemptr);
	refoff = ItemPointerGetOffsetNumberNoCheck(itemptr);

	/*
	 * Start from the root, and walk down the B-tree to find the right leaf
	 * node.
	 */
	node = (VacuumTidMapInternalNode *) dt->root;
	while (level > 0)
	{
		itemno = vac_itemptr_binsrch(refblk, refoff, node->itemptrs, node->num_items);

		if (itemno < 0)
			return false;

		node = (VacuumTidMapInternalNode *) node->downlinks[itemno];
		level--;
	}

	leaf_node = (VacuumTidMapLeafNode *) node;
	Assert(leaf_node->level == 0);

	/* Binary search in the leaf node */
	itemno = vac_itemptr_binsrch(refblk, refoff, leaf_node->itemptrs, leaf_node->num_items);

	if (itemno < 0)
		return false;
	else
	{
		ItemPointer titem = &leaf_node->itemptrs[itemno];

		return (refblk == ItemPointerGetBlockNumberNoCheck(titem) &&
				refoff == ItemPointerGetOffsetNumberNoCheck(titem));
	}
}


/*
 * vac_itemptr_binsrch() -- search a sorted array of item pointers
 *
 * Returns the offset of the first item pointer less than or equal to
 * 'refblk' and 'refoff', or -1 if there is no such item pointer.
 *
 * All item pointers in the array are assumed to be valid
 */
static inline int
vac_itemptr_binsrch(BlockNumber refblk, OffsetNumber refoff,
					ItemPointer arr, int arr_elems)
{
	BlockNumber blk;
	OffsetNumber off;
	ItemPointer value;
	int			left,
				right,
				mid;

	left = 0;
	right = arr_elems;
	while (right > left)
	{
		/*
		 * We're dealing with indexes in the B-tree nodes, which are orders of
		 * magnitude smaller than the max range of int, so we don't need to
		 * worry about overflow here.
		 */
		mid = (left + right) / 2;
		value = &arr[mid];

		blk = ItemPointerGetBlockNumberNoCheck(value);
		if (refblk < blk)
		{
			right = mid;
		}
		else if (refblk == blk)
		{
			off = ItemPointerGetOffsetNumberNoCheck(value);
			if (refoff < off)
				right = mid;
			else if (refoff == off)
				return mid;
			else
				left = mid + 1;
		}
		else
		{
			left = mid + 1;
		}
	}

	return left - 1;
}
