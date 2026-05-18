/*-------------------------------------------------------------------------
 *
 * anyarray_gin.c
 *		GIN opclasses that add the @@ (anyarray, anyquery) boolean search
 *		operator to the existing array_ops behaviour.
 *
 * GIN's extractQuery support function doesn't see fn_expr, so the element
 * type cannot be derived dynamically when the query is an anyquery (which
 * carries only text tokens).  We work around this by exposing one opclass
 * per concrete element type; each is a thin C wrapper that dispatches on
 * the strategy number, delegating standard operators to inline replicas of
 * core's array_ops behaviour and parsing anyquery tokens via the type's
 * input function for strategy 5.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/anyarray/anyarray_gin.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "anyarray.h"

#include "access/gin.h"
#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

PG_FUNCTION_INFO_V1(anyarray_gin_extract_query_int8);
PG_FUNCTION_INFO_V1(anyarray_gin_extract_query_uuid);
PG_FUNCTION_INFO_V1(anyarray_gin_extract_query_text);
PG_FUNCTION_INFO_V1(anyarray_gin_consistent_int8);
PG_FUNCTION_INFO_V1(anyarray_gin_consistent_uuid);
PG_FUNCTION_INFO_V1(anyarray_gin_consistent_text);


/* ------------------------------------------------------------------------
 *  Common: extractQuery dispatcher
 * ------------------------------------------------------------------------
 */

/*
 * Extract array elements as GIN keys (used for strategies 1-4).
 *
 * Mirrors core's ginqueryarrayextract logic, but inlined so we don't depend
 * on it being callable.  Returns a freshly-palloc'd Datum vector.
 */
static Datum *
extract_array_keys(ArrayType *arr, AnyArrayTypeInfo *meta,
				   int32 *nentries, bool **nulls_out)
{
	Datum	   *values;
	bool	   *nulls;
	int			n;
	int			i;
	int			j = 0;

	if (ARR_NDIM(arr) == 0)
	{
		*nentries = 0;
		*nulls_out = NULL;
		return NULL;
	}

	deconstruct_array(arr, meta->element_type, meta->typlen,
					  meta->typbyval, meta->typalign,
					  &values, &nulls, &n);

	/* compact out NULL entries (we treat NULLs as never-present) */
	for (i = 0; i < n; i++)
	{
		if (!nulls[i])
		{
			if (j != i)
			{
				values[j] = values[i];
				nulls[j] = false;
			}
			j++;
		}
	}

	*nentries = j;
	*nulls_out = nulls;
	return values;
}

/*
 * Walk the anyquery: every VAL is a key that must be looked up in the GIN
 * index.  Operators (& | !) do not contribute keys.  Returns true if at
 * least one VAL must be present for the query to possibly match (i.e. the
 * query is not e.g. "!foo").
 */
static bool
anyquery_has_required(AnyQuery *q, int idx)
{
	AnyQueryItem *it;

	if (idx < 0)
		return false;
	it = &q->items[idx];

	if (it->type == ANYQ_VAL)
		return true;
	if (it->payload == ANYQ_NOT)
		return false;			/* assume non-required under NOT */
	if (it->payload == ANYQ_AND)
		return anyquery_has_required(q, idx + it->left) ||
			anyquery_has_required(q, idx - 1);
	/* OR: both sides must contain required values */
	return anyquery_has_required(q, idx + it->left) &&
		anyquery_has_required(q, idx - 1);
}

static Datum
do_gin_extract_query(FunctionCallInfo fcinfo, Oid elem_type)
{
	Datum		queryDatum = PG_GETARG_DATUM(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strat = PG_GETARG_UINT16(2);

	/* PG_GETARG_POINTER(3): partial_matches -- not used */
	/* PG_GETARG_POINTER(4): extra_data -- not used */
	bool	  **nullFlags = (bool **) PG_GETARG_POINTER(5);
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	Datum	   *keys = NULL;

	*nentries = 0;
	*nullFlags = NULL;
	*searchMode = GIN_SEARCH_MODE_DEFAULT;

	if (strat == ANYARRAY_GIN_BOOLEAN_STRATEGY)
	{
		AnyQuery   *q = DatumGetAnyQueryP(queryDatum);
		Oid			input_func;
		Oid			input_typioparam;
		int			i;
		int			k = 0;

		if (q->size <= 0)
			PG_RETURN_POINTER(NULL);

		/*
		 * If the query has no required VAL (e.g. just "!foo"), we must scan
		 * the whole index because rows containing NONE of the queried values
		 * are valid matches.
		 */
		if (!anyquery_has_required(q, q->size - 1))
			*searchMode = GIN_SEARCH_MODE_ALL;

		getTypeInputInfo(elem_type, &input_func, &input_typioparam);
		keys = (Datum *) palloc(sizeof(Datum) * q->size);

		for (i = 0; i < q->size; i++)
		{
			AnyQueryItem *it = &q->items[i];

			if (it->type != ANYQ_VAL)
				continue;
			keys[k++] = OidInputFunctionCall(input_func,
											 (char *) ANYQUERY_STRING(q, it),
											 input_typioparam, -1);
		}
		*nentries = k;
		PG_RETURN_POINTER(keys);
	}
	else
	{
		ArrayType  *arr = DatumGetArrayTypeP(queryDatum);
		AnyArrayTypeInfo *meta = anyarray_get_meta(fcinfo, elem_type, false);
		bool	   *nulls;

		ANYARRAY_CHECK_ARRAY(arr);
		keys = extract_array_keys(arr, meta, nentries, &nulls);

		switch (strat)
		{
			case ANYARRAY_GIN_OVERLAP_STRATEGY:
				*searchMode = GIN_SEARCH_MODE_DEFAULT;
				break;
			case ANYARRAY_GIN_CONTAINED_STRATEGY:
				*searchMode = GIN_SEARCH_MODE_INCLUDE_EMPTY;
				break;
			case ANYARRAY_GIN_EQUAL_STRATEGY:
				*searchMode = (*nentries > 0)
					? GIN_SEARCH_MODE_DEFAULT
					: GIN_SEARCH_MODE_INCLUDE_EMPTY;
				break;
			case ANYARRAY_GIN_CONTAINS_STRATEGY:
				*searchMode = (*nentries > 0)
					? GIN_SEARCH_MODE_DEFAULT
					: GIN_SEARCH_MODE_ALL;
				break;
			default:
				elog(ERROR, "anyarray_gin: unknown strategy number: %d", strat);
		}

		(void) nulls;			/* swallowed by extract_array_keys */
		PG_RETURN_POINTER(keys);
	}
}


/* ------------------------------------------------------------------------
 *  Common: consistent dispatcher
 * ------------------------------------------------------------------------
 */

/*
 * Evaluate an anyquery against an array of "key present" flags.  The i-th
 * VAL in postfix order corresponds to check[i] (we mapped them in
 * extractQuery, so the j-th VAL we emitted is check[j]).  We rebuild that
 * VAL-only mapping here as we walk the postfix tree.
 */
typedef struct AnyQueryCheck
{
	const bool *check;			/* GIN's present-key flags */
	int			next;			/* next index in check[] */
} AnyQueryCheck;

static bool
eval_with_check(AnyQuery *q, int idx, bool *vals)
{
	AnyQueryItem *it = &q->items[idx];

	if (it->type == ANYQ_VAL)
		return vals[idx];
	if (it->payload == ANYQ_NOT)
		return !eval_with_check(q, idx - 1, vals);
	if (it->payload == ANYQ_AND)
		return eval_with_check(q, idx + it->left, vals) &&
			eval_with_check(q, idx - 1, vals);
	/* OR */
	return eval_with_check(q, idx + it->left, vals) ||
		eval_with_check(q, idx - 1, vals);
}

static Datum
do_gin_consistent(FunctionCallInfo fcinfo, Oid elem_type)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strat = PG_GETARG_UINT16(1);
	Datum		queryDatum = PG_GETARG_DATUM(2);
	int32		nkeys = PG_GETARG_INT32(3);

	/* PG_GETARG_POINTER(4): extra_data -- not used */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		result = false;
	int			i;

	(void) elem_type;

	if (strat == ANYARRAY_GIN_BOOLEAN_STRATEGY)
	{
		AnyQuery   *q = DatumGetAnyQueryP(queryDatum);
		bool	   *vals;
		int			k = 0;

		*recheck = false;

		if (q->size <= 0)
			PG_RETURN_BOOL(false);

		/* Map each VAL postfix slot to its position in check[]. */
		vals = (bool *) palloc(sizeof(bool) * q->size);
		for (i = 0; i < q->size; i++)
		{
			if (q->items[i].type == ANYQ_VAL)
				vals[i] = check[k++];
		}
		result = eval_with_check(q, q->size - 1, vals);
		pfree(vals);
		PG_RETURN_BOOL(result);
	}

	switch (strat)
	{
		case ANYARRAY_GIN_OVERLAP_STRATEGY:
			*recheck = false;

			/*
			 * GIN guarantees at least one true entry on entry; safe to say
			 * yes
			 */
			for (i = 0; i < nkeys; i++)
			{
				if (check[i])
				{
					result = true;
					break;
				}
			}
			break;
		case ANYARRAY_GIN_CONTAINED_STRATEGY:
			*recheck = true;
			result = true;		/* must always recheck */
			break;
		case ANYARRAY_GIN_EQUAL_STRATEGY:
			*recheck = true;
			result = true;
			for (i = 0; i < nkeys; i++)
			{
				if (!check[i])
				{
					result = false;
					break;
				}
			}
			break;
		case ANYARRAY_GIN_CONTAINS_STRATEGY:
			*recheck = false;
			result = true;
			for (i = 0; i < nkeys; i++)
			{
				if (!check[i])
				{
					result = false;
					break;
				}
			}
			break;
		default:
			elog(ERROR, "anyarray_gin: unknown strategy number: %d", strat);
	}

	PG_RETURN_BOOL(result);
}


/* ------------------------------------------------------------------------
 *  Per-type wrappers
 * ------------------------------------------------------------------------
 */

Datum
anyarray_gin_extract_query_int8(PG_FUNCTION_ARGS)
{
	return do_gin_extract_query(fcinfo, INT8OID);
}

Datum
anyarray_gin_extract_query_uuid(PG_FUNCTION_ARGS)
{
	return do_gin_extract_query(fcinfo, UUIDOID);
}

Datum
anyarray_gin_extract_query_text(PG_FUNCTION_ARGS)
{
	return do_gin_extract_query(fcinfo, TEXTOID);
}

Datum
anyarray_gin_consistent_int8(PG_FUNCTION_ARGS)
{
	return do_gin_consistent(fcinfo, INT8OID);
}

Datum
anyarray_gin_consistent_uuid(PG_FUNCTION_ARGS)
{
	return do_gin_consistent(fcinfo, UUIDOID);
}

Datum
anyarray_gin_consistent_text(PG_FUNCTION_ARGS)
{
	return do_gin_consistent(fcinfo, TEXTOID);
}
