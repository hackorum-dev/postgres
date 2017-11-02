/*-------------------------------------------------------------------------
 *
 * arraylist.h
 *	  interface for PostgreSQL generic array lists
 *
 * AList is a generic list type which allows O(1) lookups to a known element
 * index.  Array lists are also more CPU cache friendly than a linked list,
 * however, there are also drawbacks such as removing items from the middle
 * of the list can be slow as it requires moving each subsequent element in
 * the array 1 space towards the start of the array.
 *
 * As a general rule, array lists are better than linked lists when the
 * number of items to be stored is known in advance and nothing needs to be
 * removed from the list.  The reason for this is that we can add each
 * element to the list without having to perform any pallocs.  Looping over
 * an array list should also be faster than looping over a linked list due
 * to better CPU cache locality.
 *
 * It's also important to never test for an empty AList by checking if it
 * is NULL. An AList can be preallocated to a given size and still have no
 * items stored. The correct way to test for an empty list is by checking
 * that alist_count(list) == 0.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 *
 * src/include/nodes/arraylist.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef ARRAYLIST_H
#define ARRAYLIST_H

#include "nodes/nodes.h"

typedef struct AListItem AListItem;

typedef struct AList
{
	NodeTag		type;	/* T_ArrayList, T_IntArrayList, or T_OidArrayList */
	int			count;	/* number of items contained in the list */
	int			size;	/* size of items array */
	AListItem   *items;	/* elements array */
} AList;

struct AListItem
{
	union
	{
		void	   *ptr_value;
		int			int_value;
		Oid			oid_value;
	}			data;
};

/*
 * AListIterator
 *	Used for iterating over an array list
 */
typedef struct AListIterator
{
	AListItem *curr;
	AListItem *first;
	AListItem *last;
} AListIterator;

/*
 * alist_nth
 *		Get the nth 0-based element in the list
 */
static inline void *
alist_nth(const AList *list, int n)
{
	Assert(n >= 0);
	Assert(n < list->count);
	Assert(list->type == T_ArrayList);

	return list->items[n].data.ptr_value;
}

static inline int
alist_nth_int(const AList *list, int n)
{
	Assert(n >= 0);
	Assert(n < list->count);
	Assert(list->type == T_IntArrayList);

	return list->items[n].data.int_value;
}

static inline int
alist_nth_oid(const AList *list, int n)
{
	Assert(n >= 0);
	Assert(n < list->count);
	Assert(list->type == T_OidArrayList);

	return list->items[n].data.oid_value;
}

/*
 * alist_count
 *		Returns the number of items stored in the list
 */
static inline int
alist_count(const AList *list)
{
	return list ? list->count : 0;
}

/* macros for iterating over an AList */
#define alist_foreach(i, al) \
	for ((i).first = (i).curr = &(al)->items[0], \
		 i.last = &(al)->items[(al)->count - 1]; \
		 i.curr <= i.last; \
		 i.curr++)

#define alist_forboth(i1, al1, i2, al2) \
	for ((i1).first = (i1).curr = &(al1)->items[0], \
		 (i1).last = &(al1)->items[(al1)->count - 1], \
		 (i2).first = (i2).curr = &(al2)->items[0], \
		 (i2).last = &(al2)->items[(al2)->count - 1]; \
		 (i1).curr <= (i1).last && (i2).curr <= (i2).last; \
		 (i1).curr++, (i2).curr++)

#define alist_forthree(i1, al1, i2, al2, i3, al3) \
	for ((i1).first = (i1).curr = &(al1)->items[0], \
		 (i1).last = &(al1)->items[(al1)->count - 1], \
		 (i2).first = (i2).curr = &(al2)->items[0], \
		 (i2).last = &(al2)->items[(al2)->count - 1], \
		 (i3).first = (i3).curr = &(al3)->items[0], \
		 (i3).last = &(al3)->items[(al3)->count - 1]; \
		 (i1).curr <= (i1).last && \
		 (i2).curr <= (i2).last && \
		 (i3).curr <= (i3).last; \
		 (i1).curr++, (i2).curr++, (i3).curr++)

/* Gets the list value at the current AListIterator position */
#define alist_curr(i)		(i).curr->data.ptr_value
#define alist_curr_int(i)	(i).curr->data.int_value
#define alist_curr_oid(i)	(i).curr->data.oid_value

/* Gets the list index at the current AListIterator position */
#define alist_iterator_index(i) ((i).curr - (i).first)

extern AList *alist_premake(int size);
extern AList *alist_premake_int(int size);
extern AList *alist_premake_oid(int size);

extern AList *alist_add(AList *list, void *datum);
extern AList *alist_add_int(AList *list, int datum);
extern AList *alist_add_oid(AList *list, Oid datum);

extern void alist_test();

#endif							/* ARRAYLIST_H */
