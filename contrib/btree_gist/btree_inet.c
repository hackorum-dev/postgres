/*
 * contrib/btree_gist/btree_inet.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_var.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/inet.h"

/*
** inet ops
*/
PG_FUNCTION_INFO_V1(gbt_inet_compress);
PG_FUNCTION_INFO_V1(gbt_inet_union);
PG_FUNCTION_INFO_V1(gbt_inet_picksplit);
PG_FUNCTION_INFO_V1(gbt_inet_consistent);
PG_FUNCTION_INFO_V1(gbt_inet_penalty);
PG_FUNCTION_INFO_V1(gbt_inet_same);


static bool
gbt_inetgt(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	//ereport(NOTICE, (errmsg("gt")));
	return DatumGetBool(DirectFunctionCall2(network_gt,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static bool
gbt_inetge(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	//ereport(NOTICE, (errmsg("ge")));
	return DatumGetBool(DirectFunctionCall2(network_ge,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static bool
gbt_ineteq(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	//ereport(NOTICE, (errmsg("eq")));
	return DatumGetBool(DirectFunctionCall2(network_eq,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static bool
gbt_inetle(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	//ereport(NOTICE, (errmsg("le")));
	return DatumGetBool(DirectFunctionCall2(network_le,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static bool
gbt_inetlt(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	//ereport(NOTICE, (errmsg("lt")));
	return DatumGetBool(DirectFunctionCall2(network_lt,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static int32
gbt_inetkey_cmp(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	int32 x = DatumGetInt32(DirectFunctionCall2(network_cmp,
											 PointerGetDatum(a),
											 PointerGetDatum(b)));

	char *as = DatumGetCString(DirectFunctionCall1(inet_out, PointerGetDatum(a)));
	char *bs = DatumGetCString(DirectFunctionCall1(inet_out, PointerGetDatum(b)));

	//ereport(NOTICE, (errmsg("cmp %d %s %s", x, as, bs)));

	return DatumGetInt32(DirectFunctionCall2(network_cmp,
											 PointerGetDatum(a),
											 PointerGetDatum(b)));
}

static const gbtree_vinfo tinfo =
{
	gbt_t_inet,
	0,
	false,
	gbt_inetgt,
	gbt_inetge,
	gbt_ineteq,
	gbt_inetle,
	gbt_inetlt,
	gbt_inetkey_cmp,
	NULL
};


/**************************************************
 * inet ops
 **************************************************/


Datum
gbt_inet_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	GISTENTRY *e2 = gbt_var_compress(entry, &tinfo);

	PG_RETURN_POINTER(gbt_var_compress(entry, &tinfo));
}


Datum
gbt_inet_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	void	   *query = (void *) PG_GETARG_INET_PP(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	bool		retval;
	GBT_VARKEY *key = (GBT_VARKEY *) DatumGetPointer(entry->key);
	GBT_VARKEY_R r = gbt_var_key_readable(key);

	/* All cases served by this function are exact */
	*recheck = false;

	retval = gbt_var_consistent(&r, query, strategy, PG_GET_COLLATION(),
								GIST_LEAF(entry), &tinfo, fcinfo->flinfo);
	PG_RETURN_BOOL(retval);
}


Datum
gbt_inet_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int32	   *size = (int *) PG_GETARG_POINTER(1);

	PG_RETURN_POINTER(gbt_var_union(entryvec, size, PG_GET_COLLATION(),
									&tinfo, fcinfo->flinfo));
}


Datum
gbt_inet_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *o = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *n = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	PG_RETURN_POINTER(gbt_var_penalty(result, o, n, PG_GET_COLLATION(),
									  &tinfo, fcinfo->flinfo));
}

Datum
gbt_inet_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);

	gbt_var_picksplit(entryvec, v, PG_GET_COLLATION(),
					  &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(v);
}

Datum
gbt_inet_same(PG_FUNCTION_ARGS)
{
	Datum		d1 = PG_GETARG_DATUM(0);
	Datum		d2 = PG_GETARG_DATUM(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_var_same(d1, d2, PG_GET_COLLATION(), &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}
