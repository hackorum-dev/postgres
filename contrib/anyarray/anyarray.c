/*-------------------------------------------------------------------------
 *
 * anyarray.c
 *		Common code for the anyarray extension.
 *
 * This extension extends PostgreSQL with intarray-style operations and
 * index support that work for arrays of any element type with a default
 * btree opclass.  It is a generalization of the contrib/intarray module.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/anyarray/anyarray.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "anyarray.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

PG_MODULE_MAGIC_EXT(
					.name = "anyarray",
					.version = PG_VERSION
);

/*
 * anyarray_get_meta
 *		Fetch (and cache) type metadata for the given element type.
 *
 * The cache lives in fcinfo->flinfo->fn_extra so we pay the catalog lookup
 * cost only once per call site.  If "need_hash" is true the element type
 * must additionally provide a default hash function; otherwise only a btree
 * comparison function is required.
 */
AnyArrayTypeInfo *
anyarray_get_meta(FunctionCallInfo fcinfo, Oid element_type, bool need_hash)
{
	AnyArrayTypeInfo *meta = (AnyArrayTypeInfo *) fcinfo->flinfo->fn_extra;
	TypeCacheEntry *typentry;
	uint32		flags;

	if (meta == NULL)
	{
		meta = (AnyArrayTypeInfo *)
			MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,
								   sizeof(AnyArrayTypeInfo));
		fcinfo->flinfo->fn_extra = meta;
		meta->element_type = InvalidOid;
	}

	if (meta->element_type == element_type && (!need_hash || meta->have_hash))
		return meta;

	flags = TYPECACHE_CMP_PROC_FINFO | TYPECACHE_EQ_OPR_FINFO;
	if (need_hash)
		flags |= TYPECACHE_HASH_PROC_FINFO;

	typentry = lookup_type_cache(element_type, flags);

	if (!OidIsValid(typentry->cmp_proc_finfo.fn_oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not identify a comparison function for type %s",
						format_type_be(element_type))));

	if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not identify an equality operator for type %s",
						format_type_be(element_type))));

	if (need_hash && !OidIsValid(typentry->hash_proc_finfo.fn_oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not identify a hash function for type %s",
						format_type_be(element_type))));

	meta->element_type = element_type;
	get_typlenbyvalalign(element_type, &meta->typlen, &meta->typbyval,
						 &meta->typalign);
	meta->typcollation = typentry->typcollation;
	fmgr_info_cxt(typentry->cmp_proc_finfo.fn_oid, &meta->cmp_proc,
				  fcinfo->flinfo->fn_mcxt);
	fmgr_info_cxt(typentry->eq_opr_finfo.fn_oid, &meta->eq_proc,
				  fcinfo->flinfo->fn_mcxt);
	if (need_hash)
	{
		fmgr_info_cxt(typentry->hash_proc_finfo.fn_oid, &meta->hash_proc,
					  fcinfo->flinfo->fn_mcxt);
		meta->have_hash = true;
	}
	else
		meta->have_hash = false;

	return meta;
}

/*
 * anyarray_cmp_datum
 *		qsort_arg-compatible comparator delegating to the type's btree cmp.
 */
int
anyarray_cmp_datum(const void *a, const void *b, void *arg)
{
	AnyArrayTypeInfo *meta = (AnyArrayTypeInfo *) arg;
	Datum		d1 = *((const Datum *) a);
	Datum		d2 = *((const Datum *) b);
	Datum		result;

	result = FunctionCall2Coll(&meta->cmp_proc, meta->typcollation, d1, d2);
	return DatumGetInt32(result);
}
