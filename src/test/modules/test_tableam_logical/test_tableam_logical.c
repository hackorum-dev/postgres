/*--------------------------------------------------------------------------
 *
 * test_tableam_logical.c
 *		Table AMs that reuse heap's code, with and without the
 *		relation_supports_features callback.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_tableam_logical/test_tableam_logical.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_class.h"
#include "fmgr.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_tableam_decode_handler);
PG_FUNCTION_INFO_V1(test_tableam_nodecode_handler);
PG_FUNCTION_INFO_V1(test_tableam_overclaim_handler);
PG_FUNCTION_INFO_V1(test_tableam_supports_features);

static TableAmRoutine decode_methods;
static TableAmRoutine nodecode_methods;
static TableAmRoutine overclaim_methods;
static bool methods_initialized = false;

static void init_methods(void);
static uint32 overclaim_features(Relation rel, uint32 features);

/* An AM that says it supports everything, whatever it is asked. */
static uint32
overclaim_features(Relation rel, uint32 features)
{
	return ~(uint32) 0;
}

/*
 * Copies of heap's routine: the first as it is, the second without the
 * callback, the third with one that returns bits it was not asked about, to
 * show that the caller keeps only the ones it asked for.
 */
static void
init_methods(void)
{
	memcpy(&decode_methods, GetHeapamTableAmRoutine(), sizeof(TableAmRoutine));

	memcpy(&nodecode_methods, GetHeapamTableAmRoutine(), sizeof(TableAmRoutine));
	nodecode_methods.relation_supports_features = NULL;

	memcpy(&overclaim_methods, GetHeapamTableAmRoutine(), sizeof(TableAmRoutine));
	overclaim_methods.relation_supports_features = overclaim_features;

	methods_initialized = true;
}

Datum
test_tableam_decode_handler(PG_FUNCTION_ARGS)
{
	if (!methods_initialized)
		init_methods();
	PG_RETURN_POINTER(&decode_methods);
}

Datum
test_tableam_overclaim_handler(PG_FUNCTION_ARGS)
{
	if (!methods_initialized)
		init_methods();
	PG_RETURN_POINTER(&overclaim_methods);
}

Datum
test_tableam_nodecode_handler(PG_FUNCTION_ARGS)
{
	if (!methods_initialized)
		init_methods();
	PG_RETURN_POINTER(&nodecode_methods);
}

/* The features of rel's table AM among those in the mask, as the core asks. */
Datum
test_tableam_supports_features(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	uint32		features = (uint32) PG_GETARG_INT32(1);
	Relation	rel;
	uint32		result;

	rel = table_open(relid, AccessShareLock);

	/* a partitioned table or a view has no table AM to ask */
	if (!RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						RelationGetRelationName(rel))));

	result = table_relation_supports_features(rel, features);
	table_close(rel, AccessShareLock);

	PG_RETURN_INT32((int32) result);
}
