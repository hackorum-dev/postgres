/*-------------------------------------------------------------------------
 *
 * anyarray_gist.c
 *		Signature-based GiST opclass for anyarray.
 *
 * Each indexed array is summarised as a fixed-size bit vector; each element
 * contributes a single bit chosen by its hash modulo the signature length.
 * Internal node keys are bitwise unions of their children, with an
 * ALLISTRUE short-circuit when every bit would be set.
 *
 * The opclass supports the array operators &&, @>, <@, = and the anyarray
 * extension's @@ operator.  Because signatures are lossy, all matches are
 * rechecked by GiST.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/anyarray/anyarray_gist.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "anyarray.h"

#include "access/gist.h"
#include "access/reloptions.h"
#include "access/stratnum.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

/* Strategy numbers; share R-tree conventions where applicable. */
#define ANYARRAY_OVERLAP_STRATEGY		3
#define ANYARRAY_CONTAINS_STRATEGY		7
#define ANYARRAY_CONTAINED_STRATEGY		8
#define ANYARRAY_EQUAL_STRATEGY			18
#define ANYARRAY_BOOLEAN_STRATEGY		20

PG_FUNCTION_INFO_V1(anyarray_gist_key_in);
PG_FUNCTION_INFO_V1(anyarray_gist_key_out);
PG_FUNCTION_INFO_V1(anyarray_gist_consistent);
PG_FUNCTION_INFO_V1(anyarray_gist_compress);
PG_FUNCTION_INFO_V1(anyarray_gist_decompress);
PG_FUNCTION_INFO_V1(anyarray_gist_union);
PG_FUNCTION_INFO_V1(anyarray_gist_same);
PG_FUNCTION_INFO_V1(anyarray_gist_penalty);
PG_FUNCTION_INFO_V1(anyarray_gist_picksplit);
PG_FUNCTION_INFO_V1(anyarray_gist_options);


/* ------------------------------------------------------------------------
 *  Storage type stubs
 *
 *  The signature key is only ever constructed internally by the index AM,
 *  so its text input/output functions reject all calls (mirroring
 *  intbig_gkey in contrib/intarray).
 * ------------------------------------------------------------------------
 */

Datum
anyarray_gist_key_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s",
					"anyarray_gist_key")));
	PG_RETURN_VOID();
}

Datum
anyarray_gist_key_out(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot display a value of type %s",
					"anyarray_gist_key")));
	PG_RETURN_VOID();
}


/* ------------------------------------------------------------------------
 *  Signature helpers
 * ------------------------------------------------------------------------
 */

static AnyArrayGistKey *
alloc_key(bool allistrue, int siglen, const unsigned char *src)
{
	int32		flag = allistrue ? ANYARRAY_ALLISTRUE : 0;
	Size		size = ANYARRAY_GKEY_SIZE(flag, siglen);
	AnyArrayGistKey *k = (AnyArrayGistKey *) palloc(size);

	SET_VARSIZE(k, size);
	k->flag = flag;
	if (!allistrue)
	{
		if (src)
			memcpy(k->data, src, siglen);
		else
			memset(k->data, 0, siglen);
	}
	return k;
}

/*
 * Set the bit corresponding to "val" (a btree hash value) in "sig", a bit
 * vector of length "siglen" bytes.
 */
static inline void
set_bit(unsigned char *sig, uint32 hashval, int siglen)
{
	uint32		bit = hashval % ((uint32) siglen * BITS_PER_BYTE);

	sig[bit / BITS_PER_BYTE] |= (1U << (bit % BITS_PER_BYTE));
}

static inline bool
get_bit(const unsigned char *sig, uint32 hashval, int siglen)
{
	uint32		bit = hashval % ((uint32) siglen * BITS_PER_BYTE);

	return (sig[bit / BITS_PER_BYTE] >> (bit % BITS_PER_BYTE)) & 1;
}

/*
 * Hash a single Datum of the given element type, returning an unsigned 32-bit
 * value suitable for indexing the bit vector.
 */
static uint32
hash_elem(AnyArrayTypeInfo *meta, Datum value)
{
	Datum		h = FunctionCall1Coll(&meta->hash_proc, meta->typcollation,
									  value);

	return (uint32) DatumGetInt32(h);
}

/*
 * Hash each element of "arr" into "sig".  The metadata must have a valid
 * hash_proc (use anyarray_get_meta(..., true)).
 */
static void
hash_array_into(unsigned char *sig, int siglen,
				ArrayType *arr, AnyArrayTypeInfo *meta)
{
	Datum	   *values;
	bool	   *nulls;
	int			nelems;
	int			i;

	if (ARR_NDIM(arr) == 0)
		return;

	deconstruct_array(arr, meta->element_type, meta->typlen,
					  meta->typbyval, meta->typalign,
					  &values, &nulls, &nelems);

	for (i = 0; i < nelems; i++)
	{
		if (!nulls[i])
			set_bit(sig, hash_elem(meta, values[i]), siglen);
	}

	pfree(values);
	pfree(nulls);
}

/* Count "1" bits in a buffer of "n" bytes. */
static int
popcount_bytes(const unsigned char *p, int n)
{
	return pg_popcount((const char *) p, n);
}


/* ------------------------------------------------------------------------
 *  compress / decompress
 * ------------------------------------------------------------------------
 */

Datum
anyarray_gist_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = entry;
	int			siglen = ANYARRAY_GET_SIGLEN();

	if (entry->leafkey)
	{
		ArrayType  *arr = DatumGetArrayTypeP(entry->key);
		AnyArrayGistKey *key;
		AnyArrayTypeInfo *meta;

		ANYARRAY_CHECK_ARRAY(arr);

		meta = anyarray_get_meta(fcinfo, ARR_ELEMTYPE(arr), true);

		key = alloc_key(false, siglen, NULL);
		hash_array_into(ANYARRAY_GKEY_SIGN(key), siglen, arr, meta);

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(key), entry->rel, entry->page,
					  entry->offset, false);
	}
	else if (!ANYARRAY_GKEY_ISALLTRUE(DatumGetPointer(entry->key)))
	{
		AnyArrayGistKey *k = (AnyArrayGistKey *) DatumGetPointer(entry->key);

		/*
		 * If every bit happens to be set, switch to ALLISTRUE storage so
		 * subsequent operations don't have to compare a full bit vector.
		 */
		if (popcount_bytes(ANYARRAY_GKEY_SIGN(k), siglen) ==
			siglen * BITS_PER_BYTE)
		{
			AnyArrayGistKey *r = alloc_key(true, siglen, NULL);

			retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
			gistentryinit(*retval, PointerGetDatum(r), entry->rel, entry->page,
						  entry->offset, false);
		}
	}

	PG_RETURN_POINTER(retval);
}

Datum
anyarray_gist_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}


/* ------------------------------------------------------------------------
 *  union / same / penalty / picksplit / options
 * ------------------------------------------------------------------------
 */

Datum
anyarray_gist_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *sizep = (int *) PG_GETARG_POINTER(1);
	int			siglen = ANYARRAY_GET_SIGLEN();
	AnyArrayGistKey *out;
	unsigned char *sig;
	int			n = entryvec->n;
	int			i;
	bool		all = false;

	out = alloc_key(false, siglen, NULL);
	sig = ANYARRAY_GKEY_SIGN(out);

	for (i = 0; i < n; i++)
	{
		AnyArrayGistKey *k = (AnyArrayGistKey *)
			DatumGetPointer(entryvec->vector[i].key);

		if (ANYARRAY_GKEY_ISALLTRUE(k))
		{
			all = true;
			break;
		}
		for (int j = 0; j < siglen; j++)
			sig[j] |= ANYARRAY_GKEY_SIGN(k)[j];
	}

	if (all || popcount_bytes(sig, siglen) == siglen * BITS_PER_BYTE)
	{
		pfree(out);
		out = alloc_key(true, siglen, NULL);
	}

	*sizep = VARSIZE(out);
	PG_RETURN_POINTER(out);
}

Datum
anyarray_gist_same(PG_FUNCTION_ARGS)
{
	AnyArrayGistKey *a = (AnyArrayGistKey *) PG_GETARG_POINTER(0);
	AnyArrayGistKey *b = (AnyArrayGistKey *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);
	int			siglen = ANYARRAY_GET_SIGLEN();

	if (ANYARRAY_GKEY_ISALLTRUE(a) || ANYARRAY_GKEY_ISALLTRUE(b))
		*result = ANYARRAY_GKEY_ISALLTRUE(a) && ANYARRAY_GKEY_ISALLTRUE(b);
	else
		*result = (memcmp(ANYARRAY_GKEY_SIGN(a), ANYARRAY_GKEY_SIGN(b),
						  siglen) == 0);

	PG_RETURN_POINTER(result);
}

/*
 * Hamming weight of (orig OR new) - Hamming weight of orig: the number of
 * new bits a child would introduce.  ALLISTRUE entries have the maximum
 * possible weight (siglen*8) so the answer is 0.
 */
Datum
anyarray_gist_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);
	int			siglen = ANYARRAY_GET_SIGLEN();
	AnyArrayGistKey *orig = (AnyArrayGistKey *) DatumGetPointer(origentry->key);
	AnyArrayGistKey *new_ = (AnyArrayGistKey *) DatumGetPointer(newentry->key);

	if (ANYARRAY_GKEY_ISALLTRUE(orig))
	{
		*penalty = 0.0;
	}
	else if (ANYARRAY_GKEY_ISALLTRUE(new_))
	{
		int			orig_bits = popcount_bytes(ANYARRAY_GKEY_SIGN(orig), siglen);

		*penalty = (float) (siglen * BITS_PER_BYTE - orig_bits);
	}
	else
	{
		unsigned char *o = ANYARRAY_GKEY_SIGN(orig);
		unsigned char *n = ANYARRAY_GKEY_SIGN(new_);
		int			added = 0;

		for (int i = 0; i < siglen; i++)
		{
			unsigned char extra = n[i] & ~o[i];

			added += pg_number_of_ones[extra];
		}
		*penalty = (float) added;
	}

	PG_RETURN_POINTER(penalty);
}

/*
 * Standard signature picksplit: sort entries by Hamming weight and split
 * down the middle.  This isn't optimal but is correct and balanced; the
 * fancier Guttman-style splits used by intbig can be added later.
 */
typedef struct PickSplitEntry
{
	OffsetNumber offset;
	int			weight;
} PickSplitEntry;

static int
psplit_cmp(const void *a, const void *b)
{
	int			wa = ((const PickSplitEntry *) a)->weight;
	int			wb = ((const PickSplitEntry *) b)->weight;

	return (wa > wb) - (wa < wb);
}

static void
or_into(unsigned char *dst, const unsigned char *src, int siglen)
{
	for (int i = 0; i < siglen; i++)
		dst[i] |= src[i];
}

Datum
anyarray_gist_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	int			siglen = ANYARRAY_GET_SIGLEN();
	int			n = entryvec->n - 1;	/* entries start at index 1 */
	PickSplitEntry *items;
	AnyArrayGistKey *left,
			   *right;
	int			i;
	int			split;

	items = (PickSplitEntry *) palloc(n * sizeof(PickSplitEntry));
	for (i = 0; i < n; i++)
	{
		AnyArrayGistKey *k = (AnyArrayGistKey *)
			DatumGetPointer(entryvec->vector[i + 1].key);

		items[i].offset = (OffsetNumber) (i + 1);
		items[i].weight = ANYARRAY_GKEY_ISALLTRUE(k)
			? siglen * BITS_PER_BYTE
			: popcount_bytes(ANYARRAY_GKEY_SIGN(k), siglen);
	}
	qsort(items, n, sizeof(PickSplitEntry), psplit_cmp);

	v->spl_left = (OffsetNumber *) palloc(n * sizeof(OffsetNumber));
	v->spl_right = (OffsetNumber *) palloc(n * sizeof(OffsetNumber));
	v->spl_nleft = 0;
	v->spl_nright = 0;
	left = alloc_key(false, siglen, NULL);
	right = alloc_key(false, siglen, NULL);

	split = n / 2;
	if (split == 0)
		split = 1;
	if (split == n)
		split = n - 1;

	for (i = 0; i < split; i++)
	{
		AnyArrayGistKey *k = (AnyArrayGistKey *)
			DatumGetPointer(entryvec->vector[items[i].offset].key);

		v->spl_left[v->spl_nleft++] = items[i].offset;
		if (ANYARRAY_GKEY_ISALLTRUE(k))
		{
			pfree(left);
			left = alloc_key(true, siglen, NULL);
		}
		else if (!ANYARRAY_GKEY_ISALLTRUE(left))
			or_into(ANYARRAY_GKEY_SIGN(left), ANYARRAY_GKEY_SIGN(k), siglen);
	}
	for (; i < n; i++)
	{
		AnyArrayGistKey *k = (AnyArrayGistKey *)
			DatumGetPointer(entryvec->vector[items[i].offset].key);

		v->spl_right[v->spl_nright++] = items[i].offset;
		if (ANYARRAY_GKEY_ISALLTRUE(k))
		{
			pfree(right);
			right = alloc_key(true, siglen, NULL);
		}
		else if (!ANYARRAY_GKEY_ISALLTRUE(right))
			or_into(ANYARRAY_GKEY_SIGN(right), ANYARRAY_GKEY_SIGN(k), siglen);
	}

	v->spl_ldatum = PointerGetDatum(left);
	v->spl_rdatum = PointerGetDatum(right);

	pfree(items);
	PG_RETURN_POINTER(v);
}

/* opclass options: just the signature length, in bytes */
static void
fill_siglen_default(int *siglen)
{
	*siglen = ANYARRAY_SIGLEN_DEFAULT;
}

Datum
anyarray_gist_options(PG_FUNCTION_ARGS)
{
	local_relopts *relopts = (local_relopts *) PG_GETARG_POINTER(0);

	init_local_reloptions(relopts, sizeof(AnyArrayGistOptions));
	add_local_int_reloption(relopts, "siglen",
							"signature length in bytes",
							ANYARRAY_SIGLEN_DEFAULT, 1, ANYARRAY_SIGLEN_MAX,
							offsetof(AnyArrayGistOptions, siglen));

	(void) fill_siglen_default;
	PG_RETURN_VOID();
}


/* ------------------------------------------------------------------------
 *  consistent
 * ------------------------------------------------------------------------
 */

/*
 * For overlap: any single matching bit means the key MIGHT contain something
 * the query references.  Returns true if at least one element of "query"
 * hashes to a set bit in "sig"; ALLISTRUE keys trivially pass.
 */
static bool
sig_overlap_array(const unsigned char *sig, int siglen,
				  ArrayType *query, AnyArrayTypeInfo *meta)
{
	Datum	   *values;
	bool	   *nulls;
	int			nelems;
	int			i;
	bool		found = false;

	if (ARR_NDIM(query) == 0)
		return false;

	deconstruct_array(query, meta->element_type, meta->typlen,
					  meta->typbyval, meta->typalign,
					  &values, &nulls, &nelems);

	for (i = 0; i < nelems; i++)
	{
		if (nulls[i])
			continue;
		if (get_bit(sig, hash_elem(meta, values[i]), siglen))
		{
			found = true;
			break;
		}
	}
	pfree(values);
	pfree(nulls);
	return found;
}

/*
 * For contains: every element of the query must hash to a set bit.
 * If any required bit is missing the key cannot contain the query.
 */
static bool
sig_contains_array(const unsigned char *sig, int siglen,
				   ArrayType *query, AnyArrayTypeInfo *meta)
{
	Datum	   *values;
	bool	   *nulls;
	int			nelems;
	int			i;
	bool		ok = true;

	if (ARR_NDIM(query) == 0)
		return true;

	deconstruct_array(query, meta->element_type, meta->typlen,
					  meta->typbyval, meta->typalign,
					  &values, &nulls, &nelems);

	for (i = 0; i < nelems; i++)
	{
		if (nulls[i])
			continue;
		if (!get_bit(sig, hash_elem(meta, values[i]), siglen))
		{
			ok = false;
			break;
		}
	}
	pfree(values);
	pfree(nulls);
	return ok;
}

/*
 * Recursive postfix evaluator for sig_eval_query().
 *
 * "vals" holds the precomputed signature lookup for every VAL slot.  NOT
 * is non-restrictive under a signature (the bit could still be set by some
 * other inserted array), so we conservatively report true on negation; the
 * GiST recheck step will then filter on the real values.
 */
static bool
sig_eval_walk(AnyQuery *q, int idx, const bool *vals)
{
	AnyQueryItem *it;

	check_stack_depth();
	Assert(idx >= 0);
	it = &q->items[idx];

	if (it->type == ANYQ_VAL)
		return vals[idx];
	if (it->payload == ANYQ_NOT)
		return true;
	if (it->payload == ANYQ_AND)
		return sig_eval_walk(q, idx + it->left, vals) &&
			sig_eval_walk(q, idx - 1, vals);
	/* ANYQ_OR */
	return sig_eval_walk(q, idx + it->left, vals) ||
		sig_eval_walk(q, idx - 1, vals);
}

/*
 * Evaluate an anyquery against a signature.  Each VAL is parsed using the
 * element type's text input, hashed, then looked up in the signature.
 */
static bool
sig_eval_query(const unsigned char *sig, int siglen,
			   AnyQuery *q, AnyArrayTypeInfo *meta,
			   Oid input_func, Oid input_typioparam)
{
	bool	   *vals;
	bool		result;
	int			i;

	if (q->size <= 0)
		return false;

	vals = (bool *) palloc0(sizeof(bool) * q->size);

	for (i = 0; i < q->size; i++)
	{
		AnyQueryItem *it = &q->items[i];
		Datum		v;

		if (it->type != ANYQ_VAL)
			continue;
		v = OidInputFunctionCall(input_func,
								 (char *) ANYQUERY_STRING(q, it),
								 input_typioparam, -1);
		vals[i] = get_bit(sig, hash_elem(meta, v), siglen);
	}

	result = sig_eval_walk(q, q->size - 1, vals);
	pfree(vals);
	return result;
}

Datum
anyarray_gist_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	StrategyNumber strat = (StrategyNumber) PG_GETARG_UINT16(2);
	Oid			subtype = PG_GETARG_OID(3);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	AnyArrayGistKey *key = (AnyArrayGistKey *) DatumGetPointer(entry->key);
	int			siglen = ANYARRAY_GET_SIGLEN();
	bool		result;

	/* All signature-based answers require a recheck. */
	*recheck = true;

	if (ANYARRAY_GKEY_ISALLTRUE(key))
	{
		/* Cannot prune anything from an all-true signature. */
		PG_RETURN_BOOL(true);
	}

	(void) subtype;

	if (strat == ANYARRAY_BOOLEAN_STRATEGY)
	{
		AnyQuery   *q = PG_GETARG_ANYQUERY_P(1);
		AnyArrayTypeInfo *meta;
		Form_pg_index ind;
		Oid			coltype;
		Oid			elemtype;
		Oid			input_func;
		Oid			input_typioparam;

		/*
		 * The query is anyquery, so we recover the element type from the
		 * indexed column's pg_index entry.  The index's own rd_att gives the
		 * STORAGE type (anyarray_gist_key), not the original array type, so
		 * we read the indrelid + indkey instead.
		 */
		ind = entry->rel->rd_index;
		if (ind == NULL || ind->indnatts < 1)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("anyarray GiST index has no key column")));
		coltype = get_atttype(ind->indrelid, ind->indkey.values[0]);
		elemtype = get_element_type(coltype);
		if (!OidIsValid(elemtype))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot determine element type for anyquery match")));

		meta = anyarray_get_meta(fcinfo, elemtype, true);
		getTypeInputInfo(elemtype, &input_func, &input_typioparam);

		result = sig_eval_query(ANYARRAY_GKEY_SIGN(key), siglen, q, meta,
								input_func, input_typioparam);
		PG_FREE_IF_COPY(q, 1);
		PG_RETURN_BOOL(result);
	}
	else
	{
		ArrayType  *query = PG_GETARG_ARRAYTYPE_P(1);
		AnyArrayTypeInfo *meta;

		ANYARRAY_CHECK_ARRAY(query);
		meta = anyarray_get_meta(fcinfo, ARR_ELEMTYPE(query), true);

		switch (strat)
		{
			case ANYARRAY_OVERLAP_STRATEGY:
				result = sig_overlap_array(ANYARRAY_GKEY_SIGN(key), siglen,
										   query, meta);
				break;
			case ANYARRAY_CONTAINS_STRATEGY:
				result = sig_contains_array(ANYARRAY_GKEY_SIGN(key), siglen,
											query, meta);
				break;
			case ANYARRAY_CONTAINED_STRATEGY:

				/*
				 * For "key <@ query", signatures cannot exclude a non-leaf
				 * key because its bits may come from many distinct children
				 * which need not individually be contained in query. Always
				 * recheck.
				 */
				if (GIST_LEAF(entry))
				{
					/*
					 * At the leaf, every set bit must also have been set by
					 * the query (i.e., element hashes to that bit).
					 */
					unsigned char *qsig;
					int			i;

					qsig = (unsigned char *) palloc0(siglen);
					hash_array_into(qsig, siglen, query, meta);
					result = true;
					for (i = 0; i < siglen; i++)
					{
						if (ANYARRAY_GKEY_SIGN(key)[i] & ~qsig[i])
						{
							result = false;
							break;
						}
					}
					pfree(qsig);
				}
				else
					result = true;
				break;
			case ANYARRAY_EQUAL_STRATEGY:

				/*
				 * The leaf signature for an equal row contains every bit of
				 * the query's hash; internal unions contain at least those
				 * bits.  So "key contains query bits" is a sound
				 * over-approximation that recheck will tighten.
				 */
				result = sig_contains_array(ANYARRAY_GKEY_SIGN(key), siglen,
											query, meta);
				break;
			default:
				elog(ERROR, "unrecognized strategy number: %d", strat);
				result = false;
				break;
		}

		PG_FREE_IF_COPY(query, 1);
		PG_RETURN_BOOL(result);
	}
}
