/*--------------------------------------------------------------------------
 *
 * testsub2.c
 *		Code for testing logical replication subscriptions.
 *
 * Copyright (c) 2018, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_subscription/testsub2.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(dummyint_in);
PG_FUNCTION_INFO_V1(dummyint_out);

/* Dummy input of data type function */
Datum
dummyint_in(PG_FUNCTION_ARGS)
{
	char   *num = PG_GETARG_CSTRING(0);
	int32	val;

	val = pg_atoi(num, sizeof(int32), '\0');
	elog(LOG, "input int: %d", val);

	PG_RETURN_INT32(val);
}

/* Dummy output of data type function */
Datum
dummyint_out(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	char	   *result = (char *) palloc(12);	/* sign, 10 digits, '\0' */

	pg_ltoa(arg1, result);
	PG_RETURN_CSTRING(result);
}
