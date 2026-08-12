/*-------------------------------------------------------------------------
 *
 * test_subxact_commit.c
 *	  Helpers to test subtransaction commit failure handling.
 *
 * Loading this module registers a SubXactCallback.  With
 * test_subxact_commit.force_error = on, the COMMIT_SUB callback raises
 * ERROR after AtSubCommit_childXids() has already published the subxact
 * XID into the parent's childXids list.  That is the window that used to
 * corrupt pg_xact when the error was caught and the outer transaction
 * later committed.  See t/001_subxact_resurrect_pk.pl.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/test/modules/test_subxact_commit/test_subxact_commit.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "fmgr.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

static bool force_commit_error = false;

static void
test_subxact_commit_cb(SubXactEvent event,
					   SubTransactionId mySubid,
					   SubTransactionId parentSubid,
					   void *arg)
{
	if (force_commit_error && event == SUBXACT_EVENT_COMMIT_SUB)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("forced error on subtransaction commit")));
}

/*
 * Dummy SQL entry point so CREATE EXTENSION loads the shared library
 * (and thus runs _PG_init) without needing shared_preload_libraries.
 */
PG_FUNCTION_INFO_V1(test_subxact_commit_init);
Datum
test_subxact_commit_init(PG_FUNCTION_ARGS)
{
	PG_RETURN_VOID();
}

void
_PG_init(void)
{
	DefineCustomBoolVariable("test_subxact_commit.force_error",
							 "Raise ERROR from SUBXACT_EVENT_COMMIT_SUB callback.",
							 NULL,
							 &force_commit_error,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	RegisterSubXactCallback(test_subxact_commit_cb, NULL);
}
