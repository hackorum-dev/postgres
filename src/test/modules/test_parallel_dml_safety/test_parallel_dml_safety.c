/*-------------------------------------------------------------------------
 *
 * test_parallel_dml_safety.c
 *	  Test code for caching of a relation's parallel DML safety hazard
 *	  level in the relcache.
 *
 * This simply exposes RelationGetParallelDmlSafety() to SQL, so that
 * regression tests can inspect the computed (and cached) hazard level of a
 * relation and verify that it is invalidated when appropriate.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/test/modules/test_parallel_dml_safety/test_parallel_dml_safety.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "fmgr.h"
#include "optimizer/clauses.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_parallel_dml_safety);
PG_FUNCTION_INFO_V1(test_parallel_dml_safety_cached);

/*
 * Return the relation's parallel DML safety hazard level ('s', 'r' or 'u'),
 * as computed and cached by RelationGetParallelDmlSafety().
 */
Datum
test_parallel_dml_safety(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	char		hazard;

	rel = table_open(relid, AccessShareLock);
	hazard = RelationGetParallelDmlSafety(rel);
	table_close(rel, AccessShareLock);

	PG_RETURN_CHAR(hazard);
}

/*
 * Return the relation's cached parallel DML safety hazard level ('s', 'r'
 * or 'u'), or '0' if it has not been computed yet.  Unlike
 * test_parallel_dml_safety(), this does not compute the value, so tests can
 * tell exactly which cached values an invalidation has discarded.
 */
Datum
test_parallel_dml_safety_cached(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	char		hazard;

	rel = table_open(relid, AccessShareLock);
	hazard = rel->rd_paralleldml;
	table_close(rel, AccessShareLock);

	PG_RETURN_CHAR(hazard == 0 ? '0' : hazard);
}
