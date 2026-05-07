/*-------------------------------------------------------------------------
 *
 * compact.c
 *	  implementation of the COMPACT command
 *
 * COMPACT relocates live tuples from high-numbered heap pages onto
 * low-numbered pages with free space and truncates the now-empty trailing
 * pages off the relation.  Unlike REPACK / CLUSTER / VACUUM FULL, it does
 * not need a second copy of the relation on disk and never holds an
 * AccessExclusiveLock except briefly during truncation.
 *
 * Internally COMPACT runs as a sequence of vacuum() invocations:
 *
 *	1. A vacuum pass with VACOPT_COMPACT set, which scans, prunes dead
 *	   tuples in the usual way, then walks pages high-to-low, calling
 *	   heap_relocate to move live tuples onto low-numbered targets and
 *	   inserting matching index entries.
 *	2. A second vacuum pass without VACOPT_COMPACT.  The first pass's
 *	   transaction has committed by now, so the dead source tuples it
 *	   left behind are visible to OldestXmin and can be pruned and
 *	   truncated in this pass.
 *	3. Optionally, an ANALYZE pass.
 *
 * Doing the work in three vacuum() calls means COMPACT is a one-shot
 * command from the user's point of view -- they do not need a follow-up
 * VACUUM to actually shrink the relation.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/compact.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/compact.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"
#include "utils/guc.h"
#include "utils/memutils.h"

void
ExecCompact(ParseState *pstate, CompactStmt *stmt, bool isTopLevel)
{
	VacuumParams params;
	BufferAccessStrategy bstrategy = NULL;
	MemoryContext vac_context;
	bool		verbose = false;
	bool		analyze = false;
	int			ring_size = -1;
	ListCell   *lc;

	/* Parse COMPACT-specific options. */
	foreach(lc, stmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "verbose") == 0)
			verbose = defGetBoolean(opt);
		else if (strcmp(opt->defname, "analyze") == 0 ||
				 strcmp(opt->defname, "analyse") == 0)
			analyze = defGetBoolean(opt);
		else if (strcmp(opt->defname, "buffer_usage_limit") == 0)
		{
			const char *hintmsg;
			int			result;
			char	   *vac_buffer_size;

			vac_buffer_size = defGetString(opt);

			if (!parse_int(vac_buffer_size, &result, GUC_UNIT_KB, &hintmsg) ||
				(result != 0 &&
				 (result < MIN_BAS_VAC_RING_SIZE_KB || result > MAX_BAS_VAC_RING_SIZE_KB)))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("%s option must be 0 or between %d kB and %d kB",
								"BUFFER_USAGE_LIMIT",
								MIN_BAS_VAC_RING_SIZE_KB, MAX_BAS_VAC_RING_SIZE_KB),
						 hintmsg ? errhint_internal("%s", _(hintmsg)) : 0));
			}

			ring_size = result;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized COMPACT option \"%s\"",
							opt->defname),
					 parser_errposition(pstate, opt->location)));
	}

	/* Build common parameters, mirroring ExecVacuum's setup. */
	memset(&params, 0, sizeof(params));
	params.index_cleanup = VACOPTVALUE_UNSPECIFIED;
	params.truncate = VACOPTVALUE_ENABLED;
	params.nworkers = -1;		/* parallel vacuum disabled for now */
	params.toast_parent = InvalidOid;
	params.freeze_min_age = -1;
	params.freeze_table_age = -1;
	params.multixact_freeze_min_age = -1;
	params.multixact_freeze_table_age = -1;
	params.is_wraparound = false;
	params.log_vacuum_min_duration = -1;
	params.log_analyze_min_duration = -1;
	params.max_eager_freeze_failure_rate = vacuum_max_eager_freeze_failure_rate;

	/* Cross-transaction memory and buffer strategy. */
	vac_context = AllocSetContextCreate(PortalContext,
										"Compact",
										ALLOCSET_DEFAULT_SIZES);
	{
		MemoryContext old_context = MemoryContextSwitchTo(vac_context);

		if (ring_size == -1)
			ring_size = VacuumBufferUsageLimit;
		bstrategy = GetAccessStrategyWithSize(BAS_VACUUM, ring_size);

		MemoryContextSwitchTo(old_context);
	}

	/*
	 * Pass 1: vacuum + compact.  Relocates tuples; leaves dead source
	 * versions on the originating pages.
	 */
	params.options = VACOPT_VACUUM | VACOPT_PROCESS_MAIN | VACOPT_PROCESS_TOAST |
		VACOPT_COMPACT |
		(verbose ? VACOPT_VERBOSE : 0);
	vacuum(stmt->rels, &params, bstrategy, vac_context, isTopLevel);

	/*
	 * Pass 2: plain vacuum.  The relocate transaction has committed by
	 * now, so the dead source tuples it left behind are eligible for
	 * pruning, after which lazy_truncate_heap can reclaim the trailing
	 * empty pages.
	 */
	params.options = VACOPT_VACUUM | VACOPT_PROCESS_MAIN | VACOPT_PROCESS_TOAST |
		(verbose ? VACOPT_VERBOSE : 0);
	vacuum(stmt->rels, &params, bstrategy, vac_context, isTopLevel);

	/* Pass 3 (optional): ANALYZE. */
	if (analyze)
	{
		params.options = VACOPT_ANALYZE | (verbose ? VACOPT_VERBOSE : 0);
		vacuum(stmt->rels, &params, bstrategy, vac_context, isTopLevel);
	}

	MemoryContextDelete(vac_context);
}
