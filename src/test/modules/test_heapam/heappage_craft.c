/*--------------------------------------------------------------------------
 *
 * heappage_craft.c
 *		Functions to craft heap pages with specific contents
 *
 * Copyright (c) 2022-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_heapam/heappage_craft.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"

/* Temporary Page image we're currently crafting */
static Page crafted_page = NULL;

static OffsetNumber heappage_craft_add_line_pointer(OffsetNumber offnum, ItemIdData itemid);

/* Initialize a new empty page. Must be called before the other functions */
PG_FUNCTION_INFO_V1(heappage_craft_new);
Datum
heappage_craft_new(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use test_heapam functions")));

	if (crafted_page == NULL)
		crafted_page = MemoryContextAlloc(TopMemoryContext, BLCKSZ);
	PageInit(crafted_page, BLCKSZ, 0);

	PG_RETURN_VOID();
}

/* Install the currently crafted page into a table */
PG_FUNCTION_INFO_V1(heappage_craft_install);
Datum
heappage_craft_install(PG_FUNCTION_ARGS)
{
	Oid			table_oid = PG_GETARG_OID(0);
	BlockNumber	blkno = PG_GETARG_UINT32(1);
	Relation	rel;
	BlockNumber numblocks;
	Buffer		buf;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use test_heapam functions")));

	rel = table_open(table_oid, AccessExclusiveLock);

	numblocks = RelationGetNumberOfBlocksInFork(rel, MAIN_FORKNUM);
	if (blkno == numblocks)
	{
		buf = ReadBufferExtended(rel, MAIN_FORKNUM, P_NEW, RBM_ZERO_AND_LOCK, NULL);
	}
	else
	{
		if (blkno >= RelationGetNumberOfBlocksInFork(rel, MAIN_FORKNUM))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("block number %u is out of range for relation \"%s\"",
							blkno, RelationGetRelationName(rel))));
		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_ZERO_AND_LOCK, NULL);
	}

	memcpy(BufferGetPage(buf), crafted_page, BLCKSZ);

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);

	table_close(rel, AccessExclusiveLock);

	PG_RETURN_VOID();
}

static void
flags_to_infomask(ArrayType *flags, uint16 *infomask, uint16 *infomask2)
{
	Datum	   *elems;
	int			nelems;

	deconstruct_array(flags, TEXTOID, -1, false, TYPALIGN_INT,
					  &elems, NULL, &nelems);
	for (int i = 0; i < nelems; i++)
	{
		char		*flag = TextDatumGetCString(elems[i]);

		if (strcmp(flag, "HEAP_HASNULL") == 0)
			(*infomask) |= HEAP_HASNULL;
		else if (strcmp(flag, "HEAP_HASVARWIDTH") == 0)
			(*infomask) |= HEAP_HASVARWIDTH;
		else if (strcmp(flag, "HEAP_HASEXTERNAL") == 0)
			(*infomask) |= HEAP_HASEXTERNAL;
		else if (strcmp(flag, "HEAP_HASOID_OLD") == 0)
			(*infomask) |= HEAP_HASOID_OLD;
		else if (strcmp(flag, "HEAP_XMAX_KEYSHR_LOCK") == 0)
			(*infomask) |= HEAP_XMAX_KEYSHR_LOCK;
		else if (strcmp(flag, "HEAP_COMBOCID") == 0)
			(*infomask) |= HEAP_COMBOCID;
		else if (strcmp(flag, "HEAP_XMAX_EXCL_LOCK") == 0)
			(*infomask) |= HEAP_XMAX_EXCL_LOCK;
		else if (strcmp(flag, "HEAP_XMAX_LOCK_ONLY") == 0)
			(*infomask) |= HEAP_XMAX_LOCK_ONLY;
		else if (strcmp(flag, "HEAP_XMIN_COMMITTED") == 0)
			(*infomask) |= HEAP_XMIN_COMMITTED;
		else if (strcmp(flag, "HEAP_XMIN_INVALID") == 0)
			(*infomask) |= HEAP_XMIN_INVALID;
		else if (strcmp(flag, "HEAP_XMAX_COMMITTED") == 0)
			(*infomask) |= HEAP_XMAX_COMMITTED;
		else if (strcmp(flag, "HEAP_XMAX_INVALID") == 0)
			(*infomask) |= HEAP_XMAX_INVALID;
		else if (strcmp(flag, "HEAP_XMAX_IS_MULTI") == 0)
			(*infomask) |= HEAP_XMAX_IS_MULTI;
		else if (strcmp(flag, "HEAP_UPDATED") == 0)
			(*infomask) |= HEAP_UPDATED;
		else if (strcmp(flag, "HEAP_MOVED_OFF") == 0)
			(*infomask) |= HEAP_MOVED_OFF;
		else if (strcmp(flag, "HEAP_MOVED_IN") == 0)
			(*infomask) |= HEAP_MOVED_IN;

		else if (strcmp(flag, " HEAP_KEYS_UPDATED") == 0)
			(*infomask2) |=  HEAP_KEYS_UPDATED;
		else if (strcmp(flag, " HEAP_HOT_UPDATED") == 0)
			(*infomask2) |=  HEAP_HOT_UPDATED;
		else if (strcmp(flag, " HEAP_ONLY_TUPLE") == 0)
			(*infomask2) |=  HEAP_ONLY_TUPLE;
		else
			elog(ERROR, "unknown heap tuple flag %s", flag);

		pfree(flag);
	}
}

/* Add a tuple to page being crafted */
PG_FUNCTION_INFO_V1(heappage_craft_add_tuple);
Datum
heappage_craft_add_tuple(PG_FUNCTION_ARGS)
{
	OffsetNumber offnum = PG_GETARG_UINT16(0);
	TransactionId xmin = PG_GETARG_TRANSACTIONID(1);
	TransactionId xmax = PG_GETARG_TRANSACTIONID(2);
	CommandId cid = PG_GETARG_UINT32(3);
	ItemPointer ctid = PG_GETARG_ITEMPOINTER(4);
	ArrayType *infoflags = PG_GETARG_ARRAYTYPE_P(5);
	Datum		data = PG_GETARG_DATUM(6);
	bool		isnull = false;
	HeapTuple	htup;
	TupleDesc	tupdesc;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use test_heapam functions")));

	tupdesc = CreateTemplateTupleDesc(1);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "data",
					   TEXTOID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	htup = heap_form_tuple(tupdesc, &data, &isnull);

	htup->t_data->t_choice.t_heap.t_xmin = xmin;
	htup->t_data->t_choice.t_heap.t_xmax = xmax;
	htup->t_data->t_choice.t_heap.t_field3.t_cid = cid;
	htup->t_data->t_ctid = *ctid;
	flags_to_infomask(infoflags,
					  &htup->t_data->t_infomask,
					  &htup->t_data->t_infomask2);
	
	if (PageAddItemExtended(crafted_page, (Item) htup->t_data, htup->t_len, offnum,
							PAI_OVERWRITE | PAI_IS_HEAP) == InvalidOffsetNumber)
		elog(ERROR, "failed to add tuple");

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(heappage_craft_add_lp_unused);
Datum
heappage_craft_add_lp_unused(PG_FUNCTION_ARGS)
{
	OffsetNumber offnum = PG_GETARG_UINT16(0);
	ItemIdData iid;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use test_heapam functions")));

	ItemIdSetUnused(&iid);
	heappage_craft_add_line_pointer(offnum, iid);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(heappage_craft_add_lp_redirect);
Datum
heappage_craft_add_lp_redirect(PG_FUNCTION_ARGS)
{
	OffsetNumber offnum = PG_GETARG_UINT16(0);
	OffsetNumber redirect_offnum = PG_GETARG_UINT16(1);
	ItemIdData iid;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use test_heapam functions")));

	ItemIdSetRedirect(&iid, redirect_offnum);
	heappage_craft_add_line_pointer(offnum, iid);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(heappage_craft_add_lp_dead);
Datum
heappage_craft_add_lp_dead(PG_FUNCTION_ARGS)
{
	OffsetNumber offnum = PG_GETARG_UINT16(0);
	ItemIdData iid;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use test_heapam functions")));

	ItemIdSetDead(&iid);
	heappage_craft_add_line_pointer(offnum, iid);

	PG_RETURN_VOID();
}

static OffsetNumber
heappage_craft_add_line_pointer(OffsetNumber offnum, ItemIdData itemid)
{
	PageHeader phdr = (PageHeader) crafted_page;
	OffsetNumber limit;
	
	/* Reject placing items beyond the first unused line pointer */
	limit = OffsetNumberNext(PageGetMaxOffsetNumber(crafted_page));
	if (offnum > limit)
	{
		elog(WARNING, "specified item offset is too large");
		return InvalidOffsetNumber;
	}
	if (offnum == limit)
	{
		uint16 new_lower = phdr->pd_lower + sizeof(ItemIdData);

		if (new_lower > phdr->pd_upper)
			return InvalidOffsetNumber;
		phdr->pd_lower = new_lower;
	}
	phdr->pd_linp[offnum - 1] = itemid;

	return offnum;
}
