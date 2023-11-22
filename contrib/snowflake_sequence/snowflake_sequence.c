/*-------------------------------------------------------------------------
 *
 * snowflake_sequence.c
 *              Set of functions for generating system-wide unique values
 *
 * Copyright (c) 2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *                snowflake_sequence/snowflake_sequence.c
 *
 * Snowflake ID is a globally unique identifier based on the time, machine ID,
 * and local sequence number. This extension adds a new type of sequence for
 * counting snowflake IDs. Currently, Snowflake ID is represented by a 64-bit
 * integer, and its format is as follows:
 *
 * [1bit - unused]
 *		+ [41bit - millisecond timestamp]
 *		+ [10bit - machie ID]
 *		+ [12bit - local sequence number]
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"


#include "catalog/namespace.h"
#include "commands/sequence.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC;

#define LOCAL_SEQUENCE_BIT_LENGTH 12
#define MACHINE_ID_BIT_LENGTH 10

#define TIMESTAMP_SHIFT_LENGTH (LOCAL_SEQUENCE_BIT_LENGTH + MACHINE_ID_BIT_LENGTH)

static int snowflake_get_local_nextval(text *name);

PG_FUNCTION_INFO_V1(snowflake_nextval_internal);

/*
 * Find a specified sequence from our schema, and get a next value.
 *
 * This function is basically ported from native nextval().
 */
static int
snowflake_get_local_nextval(text *name)
{
	RangeVar   *sequence;
	Oid			relid;

	sequence = makeRangeVar("snowflake_sequence", text_to_cstring(name), -1);

	/*
	 * XXX: This is not safe in the presence of concurrent DDL, but acquiring
	 * a lock here is more expensive than letting nextval_internal do it,
	 * since the latter maintains a cache that keeps us from hitting the lock
	 * manager more than once per transaction.  It's not clear whether the
	 * performance penalty is material in practice, but for now, we do it this
	 * way.
	 */
	relid = RangeVarGetRelid(sequence, NoLock, false);

	return (int) nextval_internal(relid, true);
}

/*
 * Construct a snowflake ID and return it.
 */
Datum
snowflake_nextval_internal(PG_FUNCTION_ARGS)
{
	text	   *name = PG_GETARG_TEXT_PP(0);
	int			machine_id;
	int64 		millisecond_time;
	int			local_nextval;
	int64		ret;

	/* Gather information used by snowflake ID */
	millisecond_time = GetCurrentTimestamp() / 1000;
	machine_id = PG_GETARG_INT32(1);
	local_nextval = snowflake_get_local_nextval(name);

	/* And construct them */
	ret = millisecond_time << (TIMESTAMP_SHIFT_LENGTH) |		  
		  machine_id << (LOCAL_SEQUENCE_BIT_LENGTH) |
		  local_nextval;

	PG_RETURN_INT64(ret);
}
