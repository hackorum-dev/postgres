/*
 * gistfuncs.c
 *		Functions to investigate the content of GiST indexes
 *
 * Copyright (c) 2014-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/pageinspect/gistfuncs.c
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/spgist.h"
#include "access/spgist_private.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "catalog/pg_am_d.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pageinspect.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"

PG_FUNCTION_INFO_V1(spgist_page_opaque_info);
PG_FUNCTION_INFO_V1(spgist_leafpage_items);
PG_FUNCTION_INFO_V1(spgist_innerpage_items);
PG_FUNCTION_INFO_V1(spgist_metapage_items);

#define IS_SPGIST(r) ((r)->rd_rel->relam == SPGIST_AM_OID)

static Page verify_gist_page(bytea *raw_page);

/*
 * Verify that the given bytea contains a GIST page or die in the attempt.
 * A pointer to the page is returned.
 */
static Page
verify_gist_page(bytea *raw_page)
{
	Page		page = get_page_from_raw(raw_page);
	SpGistPageOpaque opaq;

	if (PageIsNew(page))
		return page;

	/* verify the special space has the expected size */
	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(SpGistPageOpaque)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input page is not a valid %s page", "SpGiST"),
				 errdetail("Expected special size %d, got %d.",
						   (int) MAXALIGN(sizeof(SpGistPageOpaque)),
						   (int) PageGetSpecialSize(page))));

	opaq = SpGistPageGetOpaque(page);
	if (opaq->spgist_page_id != SPGIST_PAGE_ID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input page is not a valid %s page", "GiST"),
				 errdetail("Expected %08x, got %08x.",
						   SPGIST_PAGE_ID,
						   opaq->spgist_page_id)));

	return page;
}

Datum
spgist_page_opaque_info(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	TupleDesc	tupdesc;
	Page		page;
	HeapTuple	resultTuple;
	Datum		values[4];
	bool		nulls[4];
	Datum		flags[16];
	int			nflags = 0;
	uint16		flagbits;

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to use raw page functions"));

	page = verify_gist_page(raw_page);

	if (PageIsNew(page))
		PG_RETURN_NULL();

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* Convert the flags bitmask to an array of human-readable names */
	flagbits = SpGistPageGetOpaque(page)->flags;
	if (flagbits & SPGIST_META)
		flags[nflags++] = CStringGetTextDatum("meta");
	if (flagbits & SPGIST_DELETED)
		flags[nflags++] = CStringGetTextDatum("deleted");
	if (flagbits & SPGIST_LEAF)
		flags[nflags++] = CStringGetTextDatum("leaf");
	if (flagbits & SPGIST_NULLS)
		flags[nflags++] = CStringGetTextDatum("nulls");
	flagbits &= ~(SPGIST_META | SPGIST_DELETED | SPGIST_LEAF | SPGIST_NULLS);
	if (flagbits)
	{
		/* any flags we don't recognize are printed in hex */
		flags[nflags++] = DirectFunctionCall1(to_hex32, Int32GetDatum(flagbits));
	}

	memset(nulls, 0, sizeof(nulls));

	values[0] = LSNGetDatum(PageGetLSN(page));
	values[1] = Int16GetDatum(SpGistPageGetOpaque(page)->nPlaceholder);
	values[2] = Int16GetDatum(SpGistPageGetOpaque(page)->nRedirection);
	values[3] = PointerGetDatum(construct_array_builtin(flags, nflags, TEXTOID));

	/* Build and return the result tuple. */
	resultTuple = heap_form_tuple(tupdesc, values, nulls);

	return HeapTupleGetDatum(resultTuple);
}


Datum
spgist_metapage_items(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Page		metapage;
	uint16		flagbits;
	SpGistMetaPageData *metadata;

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to use raw page functions"));

	InitMaterializedSRF(fcinfo, 0);


	metapage = verify_gist_page(raw_page);

	flagbits = SpGistPageGetOpaque(metapage)->flags;

	if (!(flagbits & SPGIST_META))
		ereport(ERROR,
				errcode(ERRCODE_WRONG_OBJECT_TYPE),
				errmsg("input page is not a %s meta page", "SpGiST"));

	metadata = SpGistPageGetMeta(metapage);

	if (metadata->magicNumber != SPGIST_MAGIC_NUMBER)
		ereport(ERROR,
				errcode(ERRCODE_INDEX_CORRUPTED),
				errmsg("input page is not a valid SpGiST metapage"),
				errdetail("Expected special size %d, got %d.",
						  (int) SPGIST_MAGIC_NUMBER,
						  (int) metadata->magicNumber));

	for (int i = 0; i < SPGIST_CACHED_PAGES; i++)
	{
		Datum		values[3];
		bool		nulls[3];

		memset(nulls, 0, sizeof(nulls));

		values[0] = Int32GetDatum(i);
		values[1] = Int16GetDatum(metadata->lastUsedPages.cachedPage[i].blkno);
		values[2] = Int32GetDatum(metadata->lastUsedPages.cachedPage[i].freeSpace);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

Datum
spgist_leafpage_items(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	Oid			indexRelid = PG_GETARG_OID(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	indexRel;
	TupleDesc	tupdesc;
	Page		page;
	uint16		flagbits;
	bits16		printflags = 0;
	OffsetNumber maxoff = InvalidOffsetNumber;
	char	   *index_columns;

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to use raw page functions"));

	InitMaterializedSRF(fcinfo, 0);

	/* Open the index relation */
	indexRel = index_open(indexRelid, AccessShareLock);

	if (!IS_SPGIST(indexRel))
		ereport(ERROR,
				errcode(ERRCODE_WRONG_OBJECT_TYPE),
				errmsg("\"%s\" is not a %s index",
					   RelationGetRelationName(indexRel), "SpGiST"));

	page = verify_gist_page(raw_page);

	if (PageIsNew(page))
	{
		index_close(indexRel, AccessShareLock);
		PG_RETURN_NULL();
	}

	flagbits = SpGistPageGetOpaque(page)->flags;

	if (flagbits & SPGIST_META)
		ereport(ERROR,
				errcode(ERRCODE_WRONG_OBJECT_TYPE),
				errmsg("input page is not a %s leaf page", "SpGiST"),
				errhint("this appears to be %s metapage. Please use spgist_metapage_items.", "SpGiST"));

	/*
	 * Included attributes are added when dealing with leaf pages, discarded
	 * for non-leaf pages as these include only data for key attributes.
	 */
	printflags |= RULE_INDEXDEF_PRETTY;
	if (flagbits & SPGIST_LEAF)
		tupdesc = RelationGetDescr(indexRel);
	else
		ereport(ERROR,
				errcode(ERRCODE_WRONG_OBJECT_TYPE),
				errmsg("input page is not a %s leaf page", "SpGiST"),
				errhint("this appears to be %s inner page. Please use spgist_innerpage_items.", "SpGiST"));

	index_columns = pg_get_indexdef_columns_extended(indexRelid,
													 printflags);

	/* Avoid bogus PageGetMaxOffsetNumber() call with deleted pages */
	if (SpGistPageIsDeleted(page))
		elog(NOTICE, "page is deleted");
	else
		maxoff = PageGetMaxOffsetNumber(page);

	for (OffsetNumber offset = FirstOffsetNumber;
		 offset <= maxoff;
		 offset++)
	{
		Datum		values[8];
		bool		nulls[8];
		ItemId		id;
		IndexTuple	itup;
		Datum		itup_values[INDEX_MAX_KEYS];
		bool		itup_isnull[INDEX_MAX_KEYS];
		SpGistLeafTuple leafTuple;
		int			i;
		bool		hasNullsMask;
		bool		has_datums;
		char	   *tp;
		bits8	   *bp;

		id = PageGetItemId(page, offset);

		if (!ItemIdIsValid(id))
			elog(ERROR, "invalid ItemId");

		itup = (IndexTuple) PageGetItem(page, id);
		leafTuple = (SpGistLeafTuple) itup;
		hasNullsMask = SGLT_GET_HASNULLMASK(leafTuple);

		tp = (char *) leafTuple + SGLTHDRSZ(hasNullsMask);
		bp = (bits8 *) ((char *) leafTuple + sizeof(SpGistLeafTupleData));
		has_datums = false;

		index_deform_tuple_internal(tupdesc,
									itup_values, itup_isnull,
									tp, bp, hasNullsMask);

		memset(nulls, 0, sizeof(nulls));

		values[0] = UInt16GetDatum(offset);
		values[1] = ItemPointerGetDatum(&leafTuple->heapPtr);
		values[2] = UInt32GetDatum(leafTuple->size);
		values[3] = BoolGetDatum(hasNullsMask);
		values[4] = UInt16GetDatum(SGLT_GET_NEXTOFFSET(leafTuple));

		values[6] = InvalidTransactionId;

		switch (leafTuple->tupstate)
		{
			case SPGIST_LIVE:
				values[5] = CStringGetTextDatum("LIVE");
				has_datums = true;
				break;
			case SPGIST_REDIRECT:
				values[5] = CStringGetTextDatum("REDIRECT");
				values[6] = ((SpGistDeadTuple) leafTuple)->xid;
				break;
			case SPGIST_DEAD:
				values[5] = CStringGetTextDatum("DEAD");
				values[6] = ((SpGistDeadTuple) leafTuple)->xid;
				break;
			case SPGIST_PLACEHOLDER:
				values[5] = CStringGetTextDatum("PLACEHOLDER");
				values[6] = ((SpGistDeadTuple) leafTuple)->xid;
				break;
			default:
				ereport(ERROR, errcode(ERRCODE_INDEX_CORRUPTED), errmsg("malformed SpGist leaf tuple state %d", leafTuple->tupstate));
		}

		if (has_datums && index_columns)
		{
			StringInfoData buf;

			initStringInfo(&buf);
			appendStringInfo(&buf, "(%s)=(", index_columns);

			/* Most of this is copied from record_out(). */
			for (i = 0; i < tupdesc->natts; i++)
			{
				char	   *value;
				char	   *tmp;
				bool		nq = false;

				if (itup_isnull[i])
					value = "null";
				else
				{
					Oid			foutoid;
					bool		typisvarlena;
					Oid			typoid;

					typoid = TupleDescAttr(tupdesc, i)->atttypid;
					getTypeOutputInfo(typoid, &foutoid, &typisvarlena);
					value = OidOutputFunctionCall(foutoid, itup_values[i]);
				}

				if (i == IndexRelationGetNumberOfKeyAttributes(indexRel))
					appendStringInfoString(&buf, ") INCLUDE (");
				else if (i > 0)
					appendStringInfoString(&buf, ", ");

				/* Check whether we need double quotes for this value */
				nq = (value[0] == '\0');	/* force quotes for empty string */
				for (tmp = value; *tmp; tmp++)
				{
					char		ch = *tmp;

					if (ch == '"' || ch == '\\' ||
						ch == '(' || ch == ')' || ch == ',' ||
						isspace((unsigned char) ch))
					{
						nq = true;
						break;
					}
				}

				/* And emit the string */
				if (nq)
					appendStringInfoCharMacro(&buf, '"');
				for (tmp = value; *tmp; tmp++)
				{
					char		ch = *tmp;

					if (ch == '"' || ch == '\\')
						appendStringInfoCharMacro(&buf, ch);
					appendStringInfoCharMacro(&buf, ch);
				}
				if (nq)
					appendStringInfoCharMacro(&buf, '"');
			}

			appendStringInfoChar(&buf, ')');

			values[7] = CStringGetTextDatum(buf.data);
		}
		else
		{
			values[7] = (Datum) 0;
			nulls[7] = true;
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	index_close(indexRel, AccessShareLock);

	return (Datum) 0;
}


Datum
spgist_innerpage_items(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	Oid			indexRelid = PG_GETARG_OID(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	indexRel;
	TupleDesc	tupdesc;
	Page		page;
	uint16		flagbits;
	bits16		printflags = 0;
	OffsetNumber maxoff = InvalidOffsetNumber;
	char	   *index_columns;

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to use raw page functions"));

	InitMaterializedSRF(fcinfo, 0);

	/* Open the index relation */
	indexRel = index_open(indexRelid, AccessShareLock);

	if (!IS_SPGIST(indexRel))
		ereport(ERROR,
				errcode(ERRCODE_WRONG_OBJECT_TYPE),
				errmsg("\"%s\" is not a %s index",
					   RelationGetRelationName(indexRel), "SpGiST"));

	page = verify_gist_page(raw_page);

	if (PageIsNew(page))
	{
		index_close(indexRel, AccessShareLock);
		PG_RETURN_NULL();
	}

	flagbits = SpGistPageGetOpaque(page)->flags;

	if (flagbits & SPGIST_META)
		ereport(ERROR,
				errcode(ERRCODE_WRONG_OBJECT_TYPE),
				errmsg("input page is not a %s inner page", "SpGiST"),
				errhint("this appears to be %s metapage. Please use spgist_metapage_items.", "SpGiST"));

	/*
	 * Included attributes are added when dealing with leaf pages, discarded
	 * for non-leaf pages as these include only data for key attributes.
	 */
	printflags |= RULE_INDEXDEF_PRETTY;
	if (flagbits & SPGIST_LEAF)
		ereport(ERROR,
				errcode(ERRCODE_WRONG_OBJECT_TYPE),
				errmsg("input page is not a %s inner page", "SpGiST"),
				errhint("this appears to be %s leaf page. Please use spgist_leafpage_items.", "SpGiST"));

	tupdesc = CreateTupleDescTruncatedCopy(RelationGetDescr(indexRel),
										   IndexRelationGetNumberOfKeyAttributes(indexRel));
	printflags |= RULE_INDEXDEF_KEYS_ONLY;

	index_columns = pg_get_indexdef_columns_extended(indexRelid,
													 printflags);

	/* Avoid bogus PageGetMaxOffsetNumber() call with deleted pages */
	if (SpGistPageIsDeleted(page))
		elog(NOTICE, "page is deleted");
	else
		maxoff = PageGetMaxOffsetNumber(page);

	for (OffsetNumber offset = FirstOffsetNumber;
		 offset <= maxoff;
		 offset++)
	{
		Datum		values[8];
		bool		nulls[8];
		ItemId		id;
		IndexTuple	itup;
		Datum		itup_values[INDEX_MAX_KEYS];
		bool		itup_isnull[INDEX_MAX_KEYS];
		SpGistInnerTuple innerTuple;
		int			i;
		bool		hasNullsMask;
		bool		has_datums;
		char	   *tp;
		bits8	   *bp;

		id = PageGetItemId(page, offset);

		if (!ItemIdIsValid(id))
			elog(ERROR, "invalid ItemId");

		itup = (IndexTuple) PageGetItem(page, id);
		innerTuple = (SpGistInnerTuple) itup;

		memset(nulls, 0, sizeof(nulls));

		values[0] = UInt16GetDatum(offset);
		values[1] = UInt32GetDatum(innerTuple->allTheSame);
		values[2] = UInt32GetDatum(innerTuple->nNodes);
		values[3] = UInt32GetDatum(innerTuple->prefixSize);
		values[4] = UInt16GetDatum(innerTuple->size);

		switch (innerTuple->tupstate)
		{
			case SPGIST_LIVE:
				values[5] = CStringGetTextDatum("LIVE");
				has_datums = true;
				break;
			case SPGIST_REDIRECT:
				values[5] = CStringGetTextDatum("REDIRECT");
				break;
			case SPGIST_DEAD:
				values[5] = CStringGetTextDatum("DEAD");
				break;
			case SPGIST_PLACEHOLDER:
				values[5] = CStringGetTextDatum("PLACEHOLDER");
				break;
			default:
				ereport(ERROR, errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("malformed SpGist leaf tuple state %d", innerTuple->tupstate));
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	index_close(indexRel, AccessShareLock);

	return (Datum) 0;
}
