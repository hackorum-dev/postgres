/*-------------------------------------------------------------------------
 *
 *	  Radix map checker
 *
 * Copyright (c) 2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/Unicode/map_checker.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "catalog/namespace.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(encoding_dumper);

static int set_byteastr(unsigned char *buf, unsigned long val);

static int
set_byteastr(unsigned char *buf, unsigned long val)
{
	unsigned char *p = buf;

	if (val > 0xffffffff)
	{
		fprintf(stderr, "Code out of range : %ld\n", val);
		exit(1);
	}
	if (val > 0xffffff)
		*p++ = (val >> 24) & 0xff;
	if (val > 0xffff)
		*p++ = (val >> 16) & 0xff;
	if (val > 0xff)
		*p++ = (val >> 8) & 0xff;
	*p++ = val & 0xff;

	return p - buf;
}

Datum
encoding_dumper(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int	src_encoding = PG_GETARG_INT32(0);
	int	dst_encoding = PG_GETARG_INT32(1);
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	unsigned long i, max;
	Oid proc;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	if (tupdesc->natts != 2)
		elog(ERROR, "incorrect number of output arguments: %d", tupdesc->natts);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	if (src_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid source encoding: %d", src_encoding)));
	if (dst_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid destination encoding: %d", dst_encoding)));

	proc = FindDefaultConversionProc(src_encoding, dst_encoding);
	if (!OidIsValid(proc))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("default conversion function for encoding \"%s\" to \"%s\" does not exist",
						pg_encoding_to_char(src_encoding),
						pg_encoding_to_char(dst_encoding))));

	/* We don't bother checking beyond the domain */
	max = 0x1L << (pg_encoding_max_length(src_encoding) * 8);

	for (i = 0 ; i < max ; i++)
	{
		unsigned char src[16];
		unsigned char dst[16];
		Datum		values[2];
		bool		nulls[2];
		bytea	*srcval;
		bytea	*dstval;
		int srclen, dstlen, l;
		bool	failed = false;

		/* The interval here is quite arbitrary */
		if ((i & 0xffffff) == 0)
			CHECK_FOR_INTERRUPTS();

		srclen = set_byteastr(src, i);
		l = pg_verify_mbstr_len(src_encoding, (const char *)src, srclen, true);
		if (l != 1)
			continue;

		PG_TRY();
		{
			OidFunctionCall5(proc,
							 Int32GetDatum(src_encoding),
							 Int32GetDatum(dst_encoding),
							 CStringGetDatum(src),
							 CStringGetDatum(dst),
							 Int32GetDatum(srclen));
		}
		PG_CATCH();
		{
			FlushErrorState();
			failed = true;
		}
		PG_END_TRY();

		if (failed)
			continue;

		dstlen = strlen((const char *)dst);

		srcval = (bytea *) MemoryContextAlloc(per_query_ctx, srclen + VARHDRSZ);
		SET_VARSIZE(srcval, srclen + VARHDRSZ);
		memcpy(VARDATA(srcval), src, srclen);

		dstval = (bytea *) MemoryContextAlloc(per_query_ctx, dstlen + VARHDRSZ);
		SET_VARSIZE(dstval, dstlen + VARHDRSZ);
		memcpy(VARDATA(dstval), dst, dstlen);

		memset(nulls, 0, sizeof(nulls));

		values[0] = PointerGetDatum(srcval);
		values[1] = PointerGetDatum(dstval);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}
