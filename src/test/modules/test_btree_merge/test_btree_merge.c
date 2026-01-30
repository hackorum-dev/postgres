/*-------------------------------------------------------------------------
 *
 * test_btree_merge.c
 *		Unit tests for B-tree Merge Scan implementation
 *
 * This module provides SQL-callable functions to directly test the
 * merge scan algorithm without going through the planner.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

#define MAX_RESULTS 10000

/*
 * MergeScanResult - holds results from a merge scan execution
 */
typedef struct MergeScanResult
{
	int			tuples_returned;
	int64		tuples_accessed;
	int			num_prefixes;
	int			limit_count;
	/* For fetch function: collected row data */
	int32	   *prefixes;
	int32	   *suffixes;
} MergeScanResult;

/*
 * do_merge_scan - common merge scan execution
 *
 * Performs a merge scan with the given parameters and collects results.
 * If collect_rows is true, fetches and stores actual row data.
 */
static void
do_merge_scan(const char *table_name,
			  const char *index_name,
			  Datum *prefix_values,
			  bool *prefix_nulls,
			  int num_prefixes,
			  Datum suffix_start,
			  Oid suffix_type,
			  RegProcedure suffix_eq_proc,
			  RegProcedure suffix_ge_proc,
			  int limit_count,
			  bool collect_rows,
			  MergeScanResult *result)
{
	Oid			table_oid;
	Oid			index_oid;
	Relation	heap_rel;
	Relation	index_rel;
	IndexScanDesc scan;
	BTScanOpaque so;
	BTMergeScanState *merge_state;
	Snapshot	snapshot;
	Oid			suffix_cmp_oid;
	Oid			opfamily;
	const char *opfamily_name;
	int			tuples_returned = 0;
	int			max_results;

	/* Determine operator family based on suffix type */
	if (suffix_type == INT4OID)
		opfamily_name = "integer_ops";
	else if (suffix_type == TIMESTAMPOID)
		opfamily_name = "datetime_ops";
	else
		elog(ERROR, "unsupported suffix type: %u", suffix_type);

	/* Look up table and index */
	table_oid = RelnameGetRelid(table_name);
	if (!OidIsValid(table_oid))
		elog(ERROR, "table \"%s\" does not exist", table_name);

	index_oid = RelnameGetRelid(index_name);
	if (!OidIsValid(index_oid))
		elog(ERROR, "index \"%s\" does not exist", index_name);

	/* Open relations */
	heap_rel = table_open(table_oid, AccessShareLock);
	index_rel = index_open(index_oid, AccessShareLock);

	/* Get comparison function for suffix type */
	opfamily = get_opfamily_oid(BTREE_AM_OID,
								list_make1(makeString(pstrdup(opfamily_name))),
								false);
	suffix_cmp_oid = get_opfamily_proc(opfamily, suffix_type, suffix_type,
									   BTORDER_PROC);
	if (!OidIsValid(suffix_cmp_oid))
		elog(ERROR, "could not find comparison function for type %u", suffix_type);

	/* Begin index scan */
	snapshot = GetActiveSnapshot();
	scan = index_beginscan(heap_rel, index_rel, snapshot, NULL, 2, 0);

	/* Set up scan keys */
	{
		ScanKeyData keys[2];

		ScanKeyInit(&keys[0], 1, BTEqualStrategyNumber, suffix_eq_proc,
					prefix_values[0]);
		ScanKeyInit(&keys[1], 2, BTGreaterEqualStrategyNumber, suffix_ge_proc,
					suffix_start);
		index_rescan(scan, keys, 2, NULL, 0);
	}

	so = (BTScanOpaque) scan->opaque;

	/* Initialize merge scan */
	merge_state = bt_merge_init(scan, prefix_values, prefix_nulls,
								num_prefixes, 1, 2, suffix_cmp_oid, InvalidOid);
	so->mergeState = merge_state;

	/* Execute scan */
	max_results = (limit_count > 0) ? limit_count : MAX_RESULTS;

	while (tuples_returned < max_results)
	{
		CHECK_FOR_INTERRUPTS();

		if (!bt_merge_getnext(scan, ForwardScanDirection))
			break;

		if (collect_rows && result->prefixes != NULL)
		{
			/* Fetch heap tuple to get actual values */
			HeapTupleData heapTuple;
			Buffer		heapBuffer;
			bool		isnull;

			heapTuple.t_self = scan->xs_heaptid;
			if (heap_fetch(heap_rel, snapshot, &heapTuple, &heapBuffer, false))
			{
				result->prefixes[tuples_returned] =
					DatumGetInt32(heap_getattr(&heapTuple, 1,
											   RelationGetDescr(heap_rel), &isnull));
				result->suffixes[tuples_returned] =
					DatumGetInt32(heap_getattr(&heapTuple, 2,
											   RelationGetDescr(heap_rel), &isnull));
				ReleaseBuffer(heapBuffer);
			}
		}

		tuples_returned++;

		if (tuples_returned >= MAX_RESULTS)
		{
			elog(WARNING, "merge scan hit safety limit of %d tuples", MAX_RESULTS);
			break;
		}
	}

	/* Collect results before cleanup */
	result->tuples_returned = tuples_returned;
	result->tuples_accessed = merge_state->tuples_accessed;
	result->num_prefixes = num_prefixes;
	result->limit_count = limit_count;

	/* Clean up */
	bt_merge_end(merge_state);
	so->mergeState = NULL;
	index_endscan(scan);
	index_close(index_rel, AccessShareLock);
	table_close(heap_rel, AccessShareLock);
}

/*
 * build_stats_result - build the stats result tuple
 */
static Datum
build_stats_result(FunctionCallInfo fcinfo, MergeScanResult *result)
{
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	HeapTuple	tuple;
	int			max_required_fetches;

	/* Calculate expected max fetches */
	if (result->tuples_returned < result->limit_count)
		max_required_fetches = result->tuples_returned;
	else
		max_required_fetches = result->limit_count + result->num_prefixes - 1;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
						"that cannot accept type record")));

	tupdesc = BlessTupleDesc(tupdesc);

	values[0] = Int32GetDatum(result->tuples_returned);
	values[1] = Int32GetDatum((int32) result->tuples_accessed);
	values[2] = Int32GetDatum(max_required_fetches);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	return HeapTupleGetDatum(tuple);
}


/*
 * test_btree_merge_scan_int - test merge scan with integer columns
 */
PG_FUNCTION_INFO_V1(test_btree_merge_scan_int);

Datum
test_btree_merge_scan_int(PG_FUNCTION_ARGS)
{
	text	   *table_name = PG_GETARG_TEXT_PP(0);
	text	   *index_name = PG_GETARG_TEXT_PP(1);
	ArrayType  *prefix_array = PG_GETARG_ARRAYTYPE_P(2);
	int32		suffix_start = PG_GETARG_INT32(3);
	int32		limit_count = PG_GETARG_INT32(4);
	Datum	   *prefix_values;
	bool	   *prefix_nulls;
	int			num_prefixes;
	MergeScanResult result = {0};

	deconstruct_array(prefix_array, INT4OID, sizeof(int32), true, TYPALIGN_INT,
					  &prefix_values, &prefix_nulls, &num_prefixes);

	if (num_prefixes == 0)
		elog(ERROR, "prefix_values array cannot be empty");

	do_merge_scan(text_to_cstring(table_name),
				  text_to_cstring(index_name),
				  prefix_values, prefix_nulls, num_prefixes,
				  Int32GetDatum(suffix_start), INT4OID,
				  F_INT4EQ, F_INT4GE,
				  limit_count, false, &result);

	return build_stats_result(fcinfo, &result);
}


/*
 * test_btree_merge_scan_ts - test merge scan with timestamp suffix
 */
PG_FUNCTION_INFO_V1(test_btree_merge_scan_ts);

Datum
test_btree_merge_scan_ts(PG_FUNCTION_ARGS)
{
	text	   *table_name = PG_GETARG_TEXT_PP(0);
	text	   *index_name = PG_GETARG_TEXT_PP(1);
	ArrayType  *prefix_array = PG_GETARG_ARRAYTYPE_P(2);
	Timestamp	suffix_start = PG_GETARG_TIMESTAMP(3);
	int32		limit_count = PG_GETARG_INT32(4);
	Datum	   *prefix_values;
	bool	   *prefix_nulls;
	int			num_prefixes;
	MergeScanResult result = {0};

	deconstruct_array(prefix_array, INT4OID, sizeof(int32), true, TYPALIGN_INT,
					  &prefix_values, &prefix_nulls, &num_prefixes);

	if (num_prefixes == 0)
		elog(ERROR, "prefix_values array cannot be empty");

	do_merge_scan(text_to_cstring(table_name),
				  text_to_cstring(index_name),
				  prefix_values, prefix_nulls, num_prefixes,
				  TimestampGetDatum(suffix_start), TIMESTAMPOID,
				  F_INT4EQ, F_TIMESTAMP_GE,
				  limit_count, false, &result);

	return build_stats_result(fcinfo, &result);
}


/*
 * test_btree_merge_fetch_int - fetch actual rows from merge scan
 */
PG_FUNCTION_INFO_V1(test_btree_merge_fetch_int);

Datum
test_btree_merge_fetch_int(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	typedef struct
	{
		int32	   *prefixes;
		int32	   *suffixes;
		int			num_results;
		int			current_idx;
	} FetchContext;

	if (SRF_IS_FIRSTCALL())
	{
		text	   *table_name = PG_GETARG_TEXT_PP(0);
		text	   *index_name = PG_GETARG_TEXT_PP(1);
		ArrayType  *prefix_array = PG_GETARG_ARRAYTYPE_P(2);
		int32		suffix_start = PG_GETARG_INT32(3);
		int32		limit_count = PG_GETARG_INT32(4);
		Datum	   *prefix_values;
		bool	   *prefix_nulls;
		int			num_prefixes;
		MemoryContext oldcontext;
		FetchContext *fctx;
		MergeScanResult result = {0};
		TupleDesc	tupdesc;
		int			max_results;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		deconstruct_array(prefix_array, INT4OID, sizeof(int32), true, TYPALIGN_INT,
						  &prefix_values, &prefix_nulls, &num_prefixes);

		if (num_prefixes == 0)
			elog(ERROR, "prefix_values array cannot be empty");

		/* Allocate result storage */
		max_results = (limit_count > 0) ? limit_count : MAX_RESULTS;
		fctx = palloc(sizeof(FetchContext));
		fctx->prefixes = palloc(max_results * sizeof(int32));
		fctx->suffixes = palloc(max_results * sizeof(int32));
		fctx->current_idx = 0;

		/* Point result to our storage */
		result.prefixes = fctx->prefixes;
		result.suffixes = fctx->suffixes;

		do_merge_scan(text_to_cstring(table_name),
					  text_to_cstring(index_name),
					  prefix_values, prefix_nulls, num_prefixes,
					  Int32GetDatum(suffix_start), INT4OID,
					  F_INT4EQ, F_INT4GE,
					  limit_count, true, &result);

		fctx->num_results = result.tuples_returned;

		/* Build result tuple descriptor */
		tupdesc = CreateTemplateTupleDesc(2);
		TupleDescInitEntry(tupdesc, 1, "prefix_col", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "suffix_col", INT4OID, -1, 0);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->user_fctx = fctx;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();

	{
		FetchContext *fctx = funcctx->user_fctx;

		if (fctx->current_idx < fctx->num_results)
		{
			Datum		values[2];
			bool		nulls[2] = {false, false};
			HeapTuple	tuple;

			values[0] = Int32GetDatum(fctx->prefixes[fctx->current_idx]);
			values[1] = Int32GetDatum(fctx->suffixes[fctx->current_idx]);
			fctx->current_idx++;

			tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
			SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
		}
		else
		{
			SRF_RETURN_DONE(funcctx);
		}
	}
}
