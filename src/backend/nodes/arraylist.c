/*-------------------------------------------------------------------------
 *
 * arraylist.c
 *	  implementation for PostgreSQL generic array list package
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/arraylist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/arraylist.h"
#include "nodes/bitmapset.h"

/*
 * Initial number of elements to allocate for new lists where the required
 * size is not specified
 */
#define ALIST_INIT_SIZE 16


static inline AList *alist_make_size_internal(NodeTag type, int size);
static inline AList *alist_add_precheck(AList *list, NodeTag type);

/************************************************************
 * Local functions
 ************************************************************/
static inline AList *
alist_make_size_internal(NodeTag type, int size)
{
	AList	   *new_list;

	new_list = (AList *) palloc(sizeof(*new_list));
	new_list->type = type;
	new_list->count = 0;
	new_list->items = (AListItem *) palloc(sizeof(AListItem) * size);
	new_list->size = size;

	return new_list;
}

static inline AList *
alist_add_precheck(AList *list, NodeTag type)
{
	/* If the list is not allocated yet, allocate it at the default size */
	if (list == NULL)
		return alist_make_size_internal(T_ArrayList, ALIST_INIT_SIZE);
	else if (list->count >= list->size)
	{
		/*
		 * XXX do we need to be smarter here, perhaps allocating in larger
		 * increments for smaller lists, and less for larger lists?
		 * For now, just double the list size.
		 */
		list->size *= 2;
		list->items = (AListItem *) repalloc(list->items,
								sizeof(AListItem) * list->size);
	}
	return list;
}

/************************************************************
 * External functions
 ************************************************************/

/*
 * alist_premake
 *		Pre-allocate a new array list with 'size' elements.
 *
 * This is useful to do if the final size of the list is known before any
 * items are added.
 */
AList *
alist_premake(int size)
{
	return alist_make_size_internal(T_ArrayList, size);
}

AList *
alist_premake_int(int size)
{
	return alist_make_size_internal(T_IntArrayList, size);
}

AList *
alist_premake_oid(int size)
{
	return alist_make_size_internal(T_OidArrayList, size);
}

/*
 * alist_add
 *		Add a new item to the list. If the list is NULL then a new list
 *		with the default size is created, or if there is not enough space
 *		for the new item, then more space will be allocated.
 */
AList *
alist_add(AList *list, void *datum)
{
	list = alist_add_precheck(list, T_ArrayList);
	list->items[list->count++].data.ptr_value = datum;
	return list;
}

AList *
alist_add_int(AList *list, int datum)
{
	list = alist_add_precheck(list, T_IntArrayList);
	list->items[list->count++].data.int_value = datum;
	return list;
}

AList *
alist_add_oid(AList *list, Oid datum)
{
	list = alist_add_precheck(list, T_OidArrayList);
	list->items[list->count++].data.oid_value = datum;
	return list;
}

/*
 * alist_delete
 *		Bulk delete items from list by index. Each bit set in del_items
 *		marks an item to be deleted from the list.
 */
static AList *
alist_delete(AList *list, Bitmapset *del_items)
{
	int src;
	int dst;

	/* No point in looping if there are no items to delete */
	if (bms_is_empty(del_items))
		return list;

	for (src = 0, dst = 0; src < list->count; src++)
	{
		if (!bms_is_member(src, del_items))
			list->items[dst++] = list->items[src];
	}

	/* record the new size of the list */
	list->count = dst;

	return list;
}

/* just a demo to show you how to use an AList */
void alist_test()
{
	AList *al = NULL;
	AListIterator i;
	AListIterator i2;
	AListIterator i3;

	Bitmapset *del = NULL;
	int x;

	/* Add a bunch of items to the list */
	for (x = 1; x <= 32; x++)
		al = alist_add_int(al, x);

	/*
	 * Loop over each item, let's see what's in each element
	 * and we'll also mark some of them to be deleted.
	 */
	alist_foreach(i, al)
	{
		int a = alist_curr_int(i);
		if ((alist_iterator_index(i) & 1) == 0)
			del = bms_add_member(del, alist_iterator_index(i));
		elog(NOTICE, "%d", a);
	}

	/* Perform the deletion */
	al = alist_delete(al, del);

	/* check the list is as we expect after having performed the delete */
	elog(NOTICE, "list countains %d items", al->count);
	alist_foreach(i, al)
	{
		int a = alist_curr_int(i);
		elog(NOTICE, "%d", a);
	}
	elog(NOTICE, "---");
	alist_forthree(i, al, i2, al, i3, al)
	{
		int a = alist_curr_int(i);
		int b = alist_curr_int(i2);
		int c = alist_curr_int(i3);

		elog(NOTICE, "%d: %d %d %d", alist_iterator_index(i), a, b, c);
	}
}
