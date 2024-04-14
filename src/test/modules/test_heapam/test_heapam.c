/*--------------------------------------------------------------------------
 *
 * test_heapam.c
 *		Heap AM test functinos
 *
 * Copyright (c) 2022-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_heapam/test_heapam.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "commands/vacuum.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(heappage_prune_and_freeze);
Datum
heappage_prune_and_freeze(PG_FUNCTION_ARGS)
{
	Oid			table_oid = PG_GETARG_OID(0);
	BlockNumber	blkno = PG_GETARG_UINT32(1);
	Relation	rel;
	BlockNumber numblocks;
	Buffer		buf;
	VacuumParams vacuum_params;
	struct GlobalVisState *vistest;
	struct VacuumCutoffs cutoffs;
	PruneFreezeResult presult;
	OffsetNumber off_loc;
	TransactionId new_relfrozen_xid;
	MultiXactId new_relmin_mxid;
	StringInfoData sinfo;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use test_heapam functions")));

	rel = table_open(table_oid, AccessExclusiveLock);

	numblocks = RelationGetNumberOfBlocksInFork(rel, MAIN_FORKNUM);
	if (blkno >= numblocks)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("block number %u is out of range for relation \"%s\"",
						blkno, RelationGetRelationName(rel))));

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);
	LockBufferForCleanup(buf);

	memset(&vacuum_params, 0, sizeof(VacuumParams));
	vacuum_params.options = VACOPT_VACUUM | VACOPT_PROCESS_MAIN | VACOPT_FREEZE;
	vacuum_params.freeze_min_age = 0;
	vacuum_params.multixact_freeze_min_age = 0;

	(void) vacuum_get_cutoffs(rel, &vacuum_params, &cutoffs);
	vistest = GlobalVisTestFor(rel);

	new_relfrozen_xid = cutoffs.OldestXmin;
	new_relmin_mxid = cutoffs.OldestMxact;
	heap_page_prune_and_freeze(rel, buf, vistest, HEAP_PAGE_PRUNE_FREEZE,
							   &cutoffs, &presult,
							   PRUNE_VACUUM_SCAN, &off_loc,
							   &new_relfrozen_xid, &new_relmin_mxid);

	initStringInfo(&sinfo);
	appendStringInfo(&sinfo, "prune results:\n");
	appendStringInfo(&sinfo, "  ndeleted: %d\n", presult.ndeleted);
	appendStringInfo(&sinfo, "  nnewlpdead: %d\n", presult.nnewlpdead);
	appendStringInfo(&sinfo, "  nfrozen: %d\n", presult.nfrozen);
	appendStringInfo(&sinfo, "  live_tuples: %d\n", presult.live_tuples);
	appendStringInfo(&sinfo, "  recently_dead_tuples: %d\n", presult.recently_dead_tuples);

	appendStringInfo(&sinfo, "  all_visible: %d\n", presult.all_visible);
	appendStringInfo(&sinfo, "  all_frozen: %d\n", presult.all_frozen);
	appendStringInfo(&sinfo, "  vm_conflict_horizon: %u\n", presult.vm_conflict_horizon);
	appendStringInfo(&sinfo, "  hastup: %d\n", presult.hastup);
	appendStringInfo(&sinfo, "  conflict_xid: %u\n", presult.conflict_xid);

	appendStringInfoString(&sinfo, "  deadoffsets: [");
	for (int i = 0; i < presult.lpdead_items; i++)
	{
		if (i > 0)
			appendStringInfoString(&sinfo, ", ");
		appendStringInfo(&sinfo, "%d", presult.deadoffsets[i]);
	}
	appendStringInfoString(&sinfo, "]\n");

	appendStringInfo(&sinfo, "  new_relfrozen_xid: %u\n", new_relfrozen_xid);
	appendStringInfo(&sinfo, "  new_relmin_mxid: %u\n", new_relmin_mxid);

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);

	table_close(rel, AccessExclusiveLock);

	elog(NOTICE, "%s", sinfo.data);

	PG_RETURN_VOID();
}
