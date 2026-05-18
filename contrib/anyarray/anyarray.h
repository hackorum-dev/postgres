/*-------------------------------------------------------------------------
 *
 * anyarray.h
 *		Shared declarations for the anyarray extension.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/anyarray/anyarray.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ANYARRAY_H
#define ANYARRAY_H

#include "access/gist.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/typcache.h"

/*
 * Per-call cached element type metadata.
 *
 * Stored in fcinfo->flinfo->fn_extra so we look up the type once per
 * planner-level call site.  All entries are populated by anyarray_get_meta().
 */
typedef struct AnyArrayTypeInfo
{
	Oid			element_type;	/* element OID, or InvalidOid if not
								 * initialized */
	int16		typlen;
	bool		typbyval;
	char		typalign;
	Oid			typcollation;	/* default collation for comparisons */
	FmgrInfo	cmp_proc;		/* btree comparison function for element_type */
	FmgrInfo	eq_proc;		/* equality function for element_type */
	FmgrInfo	hash_proc;		/* hash function for element_type (optional) */
	bool		have_hash;		/* whether hash_proc was filled in */
} AnyArrayTypeInfo;

/* Reject the kinds of input we do not handle. */
#define ANYARRAY_CHECK_ARRAY(arr) \
	do { \
		if (ARR_NDIM(arr) > 1) \
			ereport(ERROR, \
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
					 errmsg("multidimensional arrays are not supported"))); \
		if (ARR_HASNULL(arr) && array_contains_nulls(arr)) \
			ereport(ERROR, \
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), \
					 errmsg("array must not contain nulls"))); \
	} while (0)

#define ANYARRAY_NELEMS(arr) \
	(ARR_NDIM(arr) > 0 ? ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr)) : 0)

/*
 * Initialize / refresh AnyArrayTypeInfo for a given element type.
 *
 * Allocates the struct in fcinfo->flinfo->fn_mcxt on first use; on subsequent
 * calls reuses the cache unless the element type changed.  Reports an error
 * via ereport() if the element type lacks a btree comparison operator
 * (need_hash == true additionally requires a hash function).
 */
extern AnyArrayTypeInfo *anyarray_get_meta(FunctionCallInfo fcinfo,
										   Oid element_type,
										   bool need_hash);

/*
 * Comparison helper used both as a qsort_arg callback and directly.
 * "arg" must be a valid AnyArrayTypeInfo *.
 */
extern int	anyarray_cmp_datum(const void *a, const void *b, void *arg);

/*****************************************************************************
 *  Boolean query type
 *
 *  An anyquery value is a boolean expression in postfix notation whose leaves
 *  are textual representations of values.  The element type is not known at
 *  parse time; it is supplied at @@-time by the array operand, and the value
 *  strings are parsed lazily through the element type's text input function.
 *
 *  On-disk layout:
 *
 *     +-----------------------------+
 *     | varlena header              |
 *     | int32  size      (n items)  |
 *     | int32  str_off              |  -- byte offset to string heap
 *     | int32  str_len              |
 *     | AnyQueryItem items[size]    |
 *     | <padding>                   |
 *     | char   strings[str_len]     |  -- null-terminated value strings
 *     +-----------------------------+
 *****************************************************************************/

/* item.type values */
#define ANYQ_VAL	1
#define ANYQ_OPR	2

/* operator codes (also used in the input grammar) */
#define ANYQ_AND	'&'
#define ANYQ_OR		'|'
#define ANYQ_NOT	'!'

typedef struct AnyQueryItem
{
	int16		type;			/* ANYQ_VAL or ANYQ_OPR */
	int16		left;			/* offset to left-operand item (OPR only) */
	int32		payload;		/* VAL: offset into string heap; OPR: one of
								 * ANYQ_AND/OR/NOT */
} AnyQueryItem;

typedef struct AnyQuery
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int32		size;			/* number of items */
	int32		str_off;		/* byte offset to string heap */
	int32		str_len;		/* length of string heap */
	AnyQueryItem items[FLEXIBLE_ARRAY_MEMBER];
	/* followed by null-terminated value strings */
} AnyQuery;

#define ANYQUERY_HDRSIZE			offsetof(AnyQuery, items)
#define ANYQUERY_ITEMS(q)			((q)->items)
#define ANYQUERY_STRING(q, item)	((const char *) (q) + (q)->str_off + (item)->payload)
#define ANYQUERY_MAXITEMS \
	((MaxAllocSize - ANYQUERY_HDRSIZE) / sizeof(AnyQueryItem))

#define DatumGetAnyQueryP(X)		((AnyQuery *) PG_DETOAST_DATUM(X))
#define PG_GETARG_ANYQUERY_P(n)		DatumGetAnyQueryP(PG_GETARG_DATUM(n))

/*****************************************************************************
 *  GiST signature key
 *
 *  Each indexed array is summarized as a fixed-size bit vector ("signature").
 *  Each element contributes a single bit, chosen as
 *      hash(element) mod (siglen * 8).
 *  Internal node keys are bitwise unions of their children; the ALLISTRUE
 *  flag short-circuits storage when every bit would be set.
 *****************************************************************************/

#define ANYARRAY_SIGLEN_DEFAULT		(63 * 4)	/* 252 bytes -> 2016 bits */
#define ANYARRAY_SIGLEN_MAX			GISTMaxIndexKeySize
#define ANYARRAY_ALLISTRUE			0x01

typedef struct AnyArrayGistKey
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int32		flag;
	char		data[FLEXIBLE_ARRAY_MEMBER];	/* bit vector, siglen bytes */
} AnyArrayGistKey;

typedef struct AnyArrayGistOptions
{
	int32		vl_len_;
	int			siglen;			/* signature length in bytes */
} AnyArrayGistOptions;

#define ANYARRAY_GET_SIGLEN()		(PG_HAS_OPCLASS_OPTIONS() ? \
									 ((AnyArrayGistOptions *) PG_GET_OPCLASS_OPTIONS())->siglen : \
									 ANYARRAY_SIGLEN_DEFAULT)

#define ANYARRAY_GKEY_HDR			(VARHDRSZ + sizeof(int32))
#define ANYARRAY_GKEY_SIZE(flag, siglen) \
	(ANYARRAY_GKEY_HDR + (((flag) & ANYARRAY_ALLISTRUE) ? 0 : (siglen)))
#define ANYARRAY_GKEY_ISALLTRUE(k)	(((AnyArrayGistKey *) (k))->flag & ANYARRAY_ALLISTRUE)
#define ANYARRAY_GKEY_SIGN(k)		((unsigned char *) (((AnyArrayGistKey *) (k))->data))

/* GIN strategy numbers; the first four match core's gin/array_ops */
#define ANYARRAY_GIN_OVERLAP_STRATEGY	1
#define ANYARRAY_GIN_CONTAINS_STRATEGY	2
#define ANYARRAY_GIN_CONTAINED_STRATEGY	3
#define ANYARRAY_GIN_EQUAL_STRATEGY		4
#define ANYARRAY_GIN_BOOLEAN_STRATEGY	5

#define ANYARRAY_HASHVAL(h, siglen)	((unsigned int) (h) % ((siglen) * BITS_PER_BYTE))
#define ANYARRAY_SETBIT(sig, h, siglen) \
	((sig)[(ANYARRAY_HASHVAL((h), (siglen)) / BITS_PER_BYTE)] |= \
		(1 << (ANYARRAY_HASHVAL((h), (siglen)) % BITS_PER_BYTE)))
#define ANYARRAY_GETBIT(sig, h, siglen) \
	(((sig)[(ANYARRAY_HASHVAL((h), (siglen)) / BITS_PER_BYTE)] >> \
		(ANYARRAY_HASHVAL((h), (siglen)) % BITS_PER_BYTE)) & 1)

#endif							/* ANYARRAY_H */
