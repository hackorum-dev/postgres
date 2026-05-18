/*-------------------------------------------------------------------------
 *
 * anyarray_op.c
 *		Set-style operations on arrays of any type.
 *
 * All functions accept anyarray / anyelement inputs and dispatch to the
 * element type's btree comparison function through anyarray_get_meta().
 * The operations mirror the corresponding intarray operations but are
 * type-polymorphic.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/anyarray/anyarray_op.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "anyarray.h"

#include "catalog/pg_type.h"
#include "lib/qunique.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


/*
 * deconstruct_arr_meta
 *		Pull element values out of "arr" and fill in "*meta" for its type.
 *
 * Returns Datum array in *out_values (palloc'd), count in *out_nelems.  The
 * input array must already have passed ANYARRAY_CHECK_ARRAY; we still gate
 * on it defensively because all entry points feed through this helper.
 */
static void
deconstruct_arr_meta(FunctionCallInfo fcinfo, ArrayType *arr,
					 AnyArrayTypeInfo **out_meta,
					 Datum **out_values, int *out_nelems)
{
	AnyArrayTypeInfo *meta;
	Datum	   *values;
	bool	   *nulls;
	int			nelems;

	ANYARRAY_CHECK_ARRAY(arr);

	meta = anyarray_get_meta(fcinfo, ARR_ELEMTYPE(arr), false);

	if (ARR_NDIM(arr) == 0)
	{
		*out_meta = meta;
		*out_values = NULL;
		*out_nelems = 0;
		return;
	}

	deconstruct_array(arr, meta->element_type,
					  meta->typlen, meta->typbyval, meta->typalign,
					  &values, &nulls, &nelems);
	pfree(nulls);				/* we've already rejected nulls above */

	*out_meta = meta;
	*out_values = values;
	*out_nelems = nelems;
}

/*
 * make_array_from_datums
 *		Build a 1-D array from the given Datum vector.  Empty -> empty array.
 */
static ArrayType *
make_array_from_datums(Datum *values, int nelems, AnyArrayTypeInfo *meta)
{
	if (nelems == 0)
		return construct_empty_array(meta->element_type);

	return construct_array(values, nelems, meta->element_type,
						   meta->typlen, meta->typbyval, meta->typalign);
}


/* ------------------------------------------------------------------------
 *  sort / uniq
 * ------------------------------------------------------------------------
 */

PG_FUNCTION_INFO_V1(anyarray_sort);
PG_FUNCTION_INFO_V1(anyarray_sort_dir);
PG_FUNCTION_INFO_V1(anyarray_uniq);

/*
 * Sort an array ascending.
 */
Datum
anyarray_sort(PG_FUNCTION_ARGS)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
	AnyArrayTypeInfo *meta;
	Datum	   *values;
	int			nelems;

	deconstruct_arr_meta(fcinfo, arr, &meta, &values, &nelems);

	if (nelems > 1)
		qsort_arg(values, nelems, sizeof(Datum), anyarray_cmp_datum, meta);

	PG_RETURN_ARRAYTYPE_P(make_array_from_datums(values, nelems, meta));
}

/*
 * Sort an array.  The direction text is case-insensitive and must be one of
 * 'asc' / 'ascending' or 'desc' / 'descending'.
 */
Datum
anyarray_sort_dir(PG_FUNCTION_ARGS)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
	text	   *dir = PG_GETARG_TEXT_PP(1);
	const char *dirstr = VARDATA_ANY(dir);
	int			dirlen = VARSIZE_ANY_EXHDR(dir);
	bool		ascending;
	AnyArrayTypeInfo *meta;
	Datum	   *values;
	int			nelems;

	if ((dirlen == 3 && pg_strncasecmp(dirstr, "asc", 3) == 0) ||
		(dirlen == 9 && pg_strncasecmp(dirstr, "ascending", 9) == 0))
		ascending = true;
	else if ((dirlen == 4 && pg_strncasecmp(dirstr, "desc", 4) == 0) ||
			 (dirlen == 10 && pg_strncasecmp(dirstr, "descending", 10) == 0))
		ascending = false;
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("second parameter must be \"asc\" or \"desc\"")));

	deconstruct_arr_meta(fcinfo, arr, &meta, &values, &nelems);

	if (nelems > 1)
	{
		qsort_arg(values, nelems, sizeof(Datum), anyarray_cmp_datum, meta);
		if (!ascending)
		{
			Datum	   *l = values;
			Datum	   *r = values + nelems - 1;

			while (l < r)
			{
				Datum		tmp = *l;

				*l++ = *r;
				*r-- = tmp;
			}
		}
	}

	PG_RETURN_ARRAYTYPE_P(make_array_from_datums(values, nelems, meta));
}

/*
 * Remove duplicate elements; does not require pre-sorted input.
 */
Datum
anyarray_uniq(PG_FUNCTION_ARGS)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
	AnyArrayTypeInfo *meta;
	Datum	   *values;
	int			nelems;
	int			nunique;

	deconstruct_arr_meta(fcinfo, arr, &meta, &values, &nelems);

	if (nelems > 1)
	{
		qsort_arg(values, nelems, sizeof(Datum), anyarray_cmp_datum, meta);
		nunique = qunique_arg(values, nelems, sizeof(Datum),
							  anyarray_cmp_datum, meta);
	}
	else
		nunique = nelems;

	PG_RETURN_ARRAYTYPE_P(make_array_from_datums(values, nunique, meta));
}


/* ------------------------------------------------------------------------
 *  idx / subarray / cardinality
 * ------------------------------------------------------------------------
 */

PG_FUNCTION_INFO_V1(anyarray_idx);
PG_FUNCTION_INFO_V1(anyarray_subarray);
PG_FUNCTION_INFO_V1(anyarray_subarray_to_end);
PG_FUNCTION_INFO_V1(anyarray_icount);

/*
 * Return the 1-based index of the first occurrence of "elem" in "arr",
 * or 0 if not found.
 */
Datum
anyarray_idx(PG_FUNCTION_ARGS)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
	Datum		elem = PG_GETARG_DATUM(1);
	AnyArrayTypeInfo *meta;
	Datum	   *values;
	int			nelems;
	int			i;

	deconstruct_arr_meta(fcinfo, arr, &meta, &values, &nelems);

	for (i = 0; i < nelems; i++)
	{
		Datum		eq = FunctionCall2Coll(&meta->eq_proc, meta->typcollation,
										   values[i], elem);

		if (DatumGetBool(eq))
			PG_RETURN_INT32(i + 1);
	}

	PG_RETURN_INT32(0);
}

/*
 * Common subarray extraction.  "start" is 1-based; if it is non-positive
 * the function returns an empty array.  "len" gives the maximum number of
 * elements to copy; if "have_len" is false the whole tail is returned.
 */
static ArrayType *
do_subarray(FunctionCallInfo fcinfo, ArrayType *arr,
			int32 start, int32 len, bool have_len)
{
	AnyArrayTypeInfo *meta;
	Datum	   *values;
	int			nelems;
	int			off;
	int			n;

	deconstruct_arr_meta(fcinfo, arr, &meta, &values, &nelems);

	if (start < 1 || start > nelems)
		return construct_empty_array(meta->element_type);

	off = start - 1;
	if (have_len)
	{
		if (len <= 0)
			return construct_empty_array(meta->element_type);
		n = Min(len, nelems - off);
	}
	else
		n = nelems - off;

	return make_array_from_datums(values + off, n, meta);
}

/*
 * subarray(arr, start, len)
 */
Datum
anyarray_subarray(PG_FUNCTION_ARGS)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
	int32		start = PG_GETARG_INT32(1);
	int32		len = PG_GETARG_INT32(2);

	PG_RETURN_ARRAYTYPE_P(do_subarray(fcinfo, arr, start, len, true));
}

/*
 * subarray(arr, start) -- to end
 */
Datum
anyarray_subarray_to_end(PG_FUNCTION_ARGS)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
	int32		start = PG_GETARG_INT32(1);

	PG_RETURN_ARRAYTYPE_P(do_subarray(fcinfo, arr, start, 0, false));
}

/*
 * icount: total element count, intarray-style.  This duplicates
 * built-in cardinality() but we expose it under the # operator the way
 * intarray does.
 */
Datum
anyarray_icount(PG_FUNCTION_ARGS)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);

	ANYARRAY_CHECK_ARRAY(arr);
	PG_RETURN_INT32(ANYARRAY_NELEMS(arr));
}


/* ------------------------------------------------------------------------
 *  intersect / union / difference
 * ------------------------------------------------------------------------
 */

PG_FUNCTION_INFO_V1(anyarray_intersect);
PG_FUNCTION_INFO_V1(anyarray_union);
PG_FUNCTION_INFO_V1(anyarray_union_elem);
PG_FUNCTION_INFO_V1(anyarray_difference);
PG_FUNCTION_INFO_V1(anyarray_difference_elem);

/*
 * deconstruct_two
 *		Like deconstruct_arr_meta() but for two array inputs that must share
 *		an element type.  The second array is allowed to be a different
 *		ARRAY OID (it might come from another column) as long as the element
 *		types match.
 */
static AnyArrayTypeInfo *
deconstruct_two(FunctionCallInfo fcinfo,
				ArrayType *a, ArrayType *b,
				Datum **avals, int *anelems,
				Datum **bvals, int *bnelems)
{
	AnyArrayTypeInfo *meta;
	bool	   *nulls;

	ANYARRAY_CHECK_ARRAY(a);
	ANYARRAY_CHECK_ARRAY(b);

	if (ARR_ELEMTYPE(a) != ARR_ELEMTYPE(b))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot operate on arrays of different element types"),
				 errdetail("Left operand has element type %s, right operand has %s.",
						   format_type_be(ARR_ELEMTYPE(a)),
						   format_type_be(ARR_ELEMTYPE(b)))));

	meta = anyarray_get_meta(fcinfo, ARR_ELEMTYPE(a), false);

	if (ARR_NDIM(a) == 0)
	{
		*avals = NULL;
		*anelems = 0;
	}
	else
	{
		deconstruct_array(a, meta->element_type, meta->typlen,
						  meta->typbyval, meta->typalign,
						  avals, &nulls, anelems);
		pfree(nulls);
	}

	if (ARR_NDIM(b) == 0)
	{
		*bvals = NULL;
		*bnelems = 0;
	}
	else
	{
		deconstruct_array(b, meta->element_type, meta->typlen,
						  meta->typbyval, meta->typalign,
						  bvals, &nulls, bnelems);
		pfree(nulls);
	}

	return meta;
}

/*
 * Sort + de-dup in place.  Returns the new length.
 */
static int
sort_uniq(Datum *values, int nelems, AnyArrayTypeInfo *meta)
{
	if (nelems <= 1)
		return nelems;

	qsort_arg(values, nelems, sizeof(Datum), anyarray_cmp_datum, meta);
	return qunique_arg(values, nelems, sizeof(Datum),
					   anyarray_cmp_datum, meta);
}

/*
 * Intersection: returns the sorted, deduplicated values present in both
 * inputs.
 */
Datum
anyarray_intersect(PG_FUNCTION_ARGS)
{
	ArrayType  *a = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *b = PG_GETARG_ARRAYTYPE_P(1);
	AnyArrayTypeInfo *meta;
	Datum	   *av,
			   *bv,
			   *out;
	int			an,
				bn,
				i = 0,
				j = 0,
				k = 0;

	meta = deconstruct_two(fcinfo, a, b, &av, &an, &bv, &bn);

	an = sort_uniq(av, an, meta);
	bn = sort_uniq(bv, bn, meta);

	out = (Datum *) palloc(sizeof(Datum) * Min(an, bn));
	while (i < an && j < bn)
	{
		int			cmp = anyarray_cmp_datum(&av[i], &bv[j], meta);

		if (cmp == 0)
		{
			out[k++] = av[i];
			i++;
			j++;
		}
		else if (cmp < 0)
			i++;
		else
			j++;
	}

	PG_RETURN_ARRAYTYPE_P(make_array_from_datums(out, k, meta));
}

/*
 * Union: returns the sorted, deduplicated values present in either input.
 */
Datum
anyarray_union(PG_FUNCTION_ARGS)
{
	ArrayType  *a = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *b = PG_GETARG_ARRAYTYPE_P(1);
	AnyArrayTypeInfo *meta;
	Datum	   *av,
			   *bv,
			   *out;
	int			an,
				bn,
				i = 0,
				j = 0,
				k = 0;

	meta = deconstruct_two(fcinfo, a, b, &av, &an, &bv, &bn);

	an = sort_uniq(av, an, meta);
	bn = sort_uniq(bv, bn, meta);

	out = (Datum *) palloc(sizeof(Datum) * (an + bn));
	while (i < an && j < bn)
	{
		int			cmp = anyarray_cmp_datum(&av[i], &bv[j], meta);

		if (cmp == 0)
		{
			out[k++] = av[i];
			i++;
			j++;
		}
		else if (cmp < 0)
			out[k++] = av[i++];
		else
			out[k++] = bv[j++];
	}
	while (i < an)
		out[k++] = av[i++];
	while (j < bn)
		out[k++] = bv[j++];

	PG_RETURN_ARRAYTYPE_P(make_array_from_datums(out, k, meta));
}

/*
 * array | element : add element if not already present, returning the
 * sorted, deduplicated result.
 */
Datum
anyarray_union_elem(PG_FUNCTION_ARGS)
{
	ArrayType  *a = PG_GETARG_ARRAYTYPE_P(0);
	Datum		elem = PG_GETARG_DATUM(1);
	Oid			elem_type = get_fn_expr_argtype(fcinfo->flinfo, 1);
	AnyArrayTypeInfo *meta;
	Datum	   *values;
	int			nelems;
	int			out_n;

	ANYARRAY_CHECK_ARRAY(a);

	if (ARR_ELEMTYPE(a) != elem_type)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("element type does not match array element type")));

	meta = anyarray_get_meta(fcinfo, ARR_ELEMTYPE(a), false);

	if (ARR_NDIM(a) == 0)
	{
		nelems = 0;
		values = (Datum *) palloc(sizeof(Datum));
	}
	else
	{
		bool	   *nulls;

		deconstruct_array(a, meta->element_type, meta->typlen,
						  meta->typbyval, meta->typalign,
						  &values, &nulls, &nelems);
		pfree(nulls);
		values = repalloc(values, sizeof(Datum) * (nelems + 1));
	}

	values[nelems++] = elem;

	out_n = sort_uniq(values, nelems, meta);
	PG_RETURN_ARRAYTYPE_P(make_array_from_datums(values, out_n, meta));
}

/*
 * Difference: returns sorted, deduplicated values from "a" that are not in
 * "b".
 */
Datum
anyarray_difference(PG_FUNCTION_ARGS)
{
	ArrayType  *a = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *b = PG_GETARG_ARRAYTYPE_P(1);
	AnyArrayTypeInfo *meta;
	Datum	   *av,
			   *bv,
			   *out;
	int			an,
				bn,
				i = 0,
				j = 0,
				k = 0;

	meta = deconstruct_two(fcinfo, a, b, &av, &an, &bv, &bn);

	an = sort_uniq(av, an, meta);
	bn = sort_uniq(bv, bn, meta);

	out = (Datum *) palloc(sizeof(Datum) * an);
	while (i < an && j < bn)
	{
		int			cmp = anyarray_cmp_datum(&av[i], &bv[j], meta);

		if (cmp == 0)
		{
			i++;
			j++;
		}
		else if (cmp < 0)
			out[k++] = av[i++];
		else
			j++;
	}
	while (i < an)
		out[k++] = av[i++];

	PG_RETURN_ARRAYTYPE_P(make_array_from_datums(out, k, meta));
}

/*
 * array - element : remove all occurrences of "elem", preserving order.
 * (Note: intarray returns the input order-preserved; we do the same.)
 */
Datum
anyarray_difference_elem(PG_FUNCTION_ARGS)
{
	ArrayType  *a = PG_GETARG_ARRAYTYPE_P(0);
	Datum		elem = PG_GETARG_DATUM(1);
	Oid			elem_type = get_fn_expr_argtype(fcinfo->flinfo, 1);
	AnyArrayTypeInfo *meta;
	Datum	   *values;
	int			nelems;
	int			i,
				k = 0;

	ANYARRAY_CHECK_ARRAY(a);

	if (ARR_ELEMTYPE(a) != elem_type)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("element type does not match array element type")));

	meta = anyarray_get_meta(fcinfo, ARR_ELEMTYPE(a), false);

	if (ARR_NDIM(a) == 0)
		PG_RETURN_ARRAYTYPE_P(construct_empty_array(meta->element_type));

	{
		bool	   *nulls;

		deconstruct_array(a, meta->element_type, meta->typlen,
						  meta->typbyval, meta->typalign,
						  &values, &nulls, &nelems);
		pfree(nulls);
	}

	for (i = 0; i < nelems; i++)
	{
		Datum		eq = FunctionCall2Coll(&meta->eq_proc, meta->typcollation,
										   values[i], elem);

		if (!DatumGetBool(eq))
			values[k++] = values[i];
	}

	PG_RETURN_ARRAYTYPE_P(make_array_from_datums(values, k, meta));
}
