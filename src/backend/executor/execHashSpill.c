/*
 *
 *
 * TODO:
 *
 * - need a smarter strategy to choose spill victim. Must avoid infinite looping,
 *   where the same tuples get spilled again and again.
 */

#include "postgres.h"

#include "executor/executor.h"
#include "utils/hashutils.h"
#include "utils/memutils.h"
#include "storage/buffile.h"

/*
 * Spill Set
 *
 */

#define FANOUT_SHIFT 4
#define FANOUT 16
#define LEVELS 8		/* 32 bits, 4 bits per level */
#define HASH_HIGH_MASK 0xf0000000

typedef struct SubSpillSet SubSpillSet;

/*
 * Spill files form a radix tree, based on the hash key. Whenever a sub-spillset
 * grows too large, it is split. The entries that had already been written to
 * the file for that sub-spillset are kept in the old file, but any new entries
 * are written to the child nodes instead.
 *
 * XXX: I think that doesn't do the right thing with ordered aggregates, where
 * we have to take care to feed the input rows to the aggregate in the input
 * order.
 */
struct SubSpillSet
{
	BufFile	   *file;
	int			num_children;

	SubSpillSet   *children[FANOUT];
};

struct HashSpillSet
{
	SubSpillSet root;
	uint64		max_size;
};

/*
 * When processing input:
 *
 * 1. If has table is full, choose victim.
 * 2. Call GetSpillFile() on the victim
 * 3. Dump the entry to the file returned by GetSpillFile .
 * 4. Remove entry from hash table.
 */

/*
 * To finalize:
 *
 * 1. Dump remaining entries from in-memory hash table, where !firstbatch
 * 2. Return remaining entries from in-memory hash table (with firstbatch==true)
 *
 * 3. Call ReadNextSpillFile(). Read entries from the file until it's empty.
 * 4. Load each entry to in-memory hash table.
 *
 */

HashSpillSet *
CreateHashSpillSet(int64 target_file_size)
{
	HashSpillSet *sp;

	sp = palloc0(sizeof(HashSpillSet));
	sp->max_size = target_file_size;

	return sp;
}

static SubSpillSet *
CreateSubSpillSet(void)
{
	SubSpillSet *subsp;

	subsp = palloc0(sizeof(SubSpillSet));
	subsp->file = BufFileCreateTemp(false);
	subsp->num_children = 0;

	return subsp;
}

BufFile *
GetSpillFile(HashSpillSet *sp, uint32 hash)
{
	uint32		level;
	SubSpillSet *subsp;
	uint32		hash_shifted;

	level = 0;
	subsp = &sp->root;
	hash_shifted = hash;
	for (;;)
	{
		uint32		childno;

		childno = (hash_shifted & HASH_HIGH_MASK) >> (32 - FANOUT_SHIFT);

		if (!subsp->children[childno])
		{
			subsp->num_children++;
			subsp = subsp->children[childno] = CreateSubSpillSet();
			break;
		}

		subsp = subsp->children[childno];
		if (subsp->num_children == 0)
		{
			/*
			 * We found the correct sub-spillset that this tuple belongs to.
			 *
			 * But is it too full? If so, continue to recurse into it, to create
			 * a new sub-spillsets at lower level.
			 */
			if (level == LEVELS - 1 || BufFileSize(subsp->file) < sp->max_size)
				break;
		}
		Assert(level < LEVELS - 1);

		level++;
		hash_shifted <<= FANOUT_SHIFT;
	}

	return subsp->file;
}

static BufFile *
OpenNextSpillFileRecurse(HashSpillSet *sp, SubSpillSet *thissp, bool *respill)
{
	BufFile	   *file = NULL;
	int			i;

	if (thissp->file)
	{
		file = thissp->file;
		thissp->file = NULL;
		*respill = (thissp->num_children > 0);
		return file;
	}

	for (i = 0; i < FANOUT; i++)
	{
		SubSpillSet *subsp;

		if (thissp->children[i])
		{
			subsp = thissp->children[i];

			file = OpenNextSpillFileRecurse(sp, subsp, respill);

			if (!subsp->file && subsp->num_children == 0)
			{
				/* This leaf entry is no longer needed. Prune it. */
				pfree(thissp->children[i]);
				thissp->children[i] = NULL;
				thissp->num_children--;
			}
			return file;
		}
	}
	return NULL;
}

/*
 * Open the next spill file to process.
 *
 * The spill files form a tree. This returns the spill files in an order,
 * so that a parent is always returned before its children.
 */
BufFile *
OpenNextSpillFile(HashSpillSet *sp, bool *respill)
{
	/* Scan the radix tree for next batch. */
	BufFile *file;
	file = OpenNextSpillFileRecurse(sp, &sp->root, respill);
	if (file)
	{
		BufFileSeek(file, 0, 0, SEEK_SET);
		/* XXX check error */
	}
	return file;
}
void
CloseHashSpillSet(HashSpillSet *sp)
{
	BufFile	   *file;
	bool		respill;

	while((file = OpenNextSpillFile(sp, &respill)) != NULL)
		BufFileClose(file);

	pfree(sp);
}
