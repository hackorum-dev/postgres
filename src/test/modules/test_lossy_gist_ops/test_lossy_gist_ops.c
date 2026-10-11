/*-------------------------------------------------------------------------
 *
 * test_lossy_gist_ops.c
 *	  Test GiST operator class for text that is lossy and whose equality
 *	  operator differs from the type's default equality.
 *
 * The consistent function matches everything and always asks for a recheck,
 * modelling a lossy index (like the GiST opclass for multiranges, which
 * stores only the bounding range).  The equality operator === is
 * case-insensitive, unlike the type's default equality (texteq).
 *
 * This is used by the logical replication tests to verify that when the
 * apply worker rechecks a candidate found through such an index, it compares
 * using the index's own equality semantics (the opclass's equality operator
 * and the index column's collation), not the type's default equality.
 *
 * IDENTIFICATION
 *	  src/test/modules/test_lossy_gist_ops/test_lossy_gist_ops.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist.h"
#include "fmgr.h"
#include "port.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

/* case-insensitive equality */
PG_FUNCTION_INFO_V1(lossy_text_eq_ci);
Datum
lossy_text_eq_ci(PG_FUNCTION_ARGS)
{
	text	   *a = PG_GETARG_TEXT_PP(0);
	text	   *b = PG_GETARG_TEXT_PP(1);
	bool		result = pg_strcasecmp(text_to_cstring(a),
									   text_to_cstring(b)) == 0;

	PG_RETURN_BOOL(result);
}

/* lossy: everything is a possible match, always recheck */
PG_FUNCTION_INFO_V1(lossy_text_consistent);
Datum
lossy_text_consistent(PG_FUNCTION_ARGS)
{
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);

	*recheck = true;
	PG_RETURN_BOOL(true);
}

/* any child key will do: consistent() never prunes anything */
PG_FUNCTION_INFO_V1(lossy_text_union);
Datum
lossy_text_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);

	PG_RETURN_TEXT_P(DatumGetTextPCopy(entryvec->vector[0].key));
}

PG_FUNCTION_INFO_V1(lossy_text_penalty);
Datum
lossy_text_penalty(PG_FUNCTION_ARGS)
{
	float	   *result = (float *) PG_GETARG_POINTER(2);

	*result = 0.0;
	PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(lossy_text_picksplit);
Datum
lossy_text_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber i;
	OffsetNumber maxoff = entryvec->n - 1;

	v->spl_left = (OffsetNumber *) palloc0(sizeof(OffsetNumber) * entryvec->n);
	v->spl_right = (OffsetNumber *) palloc0(sizeof(OffsetNumber) * entryvec->n);
	v->spl_nleft = v->spl_nright = 0;

	for (i = 0; i <= maxoff; i++)
	{
		if (i % 2 == 0)
			v->spl_left[v->spl_nleft++] = i;
		else
			v->spl_right[v->spl_nright++] = i;
	}

	v->spl_ldatum = entryvec->vector[0].key;
	v->spl_ldatum_exists = false;
	v->spl_rdatum = entryvec->vector[maxoff].key;
	v->spl_rdatum_exists = false;

	PG_RETURN_POINTER(v);
}

/* never identical: no deduplication */
PG_FUNCTION_INFO_V1(lossy_text_same);
Datum
lossy_text_same(PG_FUNCTION_ARGS)
{
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = false;
	PG_RETURN_POINTER(result);
}
