/*-------------------------------------------------------------------------
 *
 * pg_index_overhead.c
 *		Per-index maintenance instrumentation for EXPLAIN, built on the
 *		proposed index_insert_hook / index_insert_cleanup_hook.
 *
 * This module adds an INDEX_UPDATES option to EXPLAIN.  When used together
 * with ANALYZE on a data-modifying statement, each ModifyTable node is
 * annotated with, for every index that received tuples, the number of
 * index tuples inserted, the time spent inserting them, and the WAL and
 * shared-buffer traffic attributable to that index.  A summary of
 * heap-level modifications (including HOT update counts, which explain why
 * per-index insert counts differ from the row count) is also shown.
 *
 * Collection works by interposing on index_insert(): while an instrumented
 * statement is running, each call is timed and bracketed with snapshots of
 * pgWalUsage and pgBufferUsage, accumulated into a backend-local hash table
 * keyed by index OID.  Work deferred to index_insert_cleanup() (such as GIN
 * pending-list flushes) is attributed to the same index.  Reporting uses
 * the extensible-EXPLAIN facilities added in PostgreSQL 18.
 *
 * This is a demonstration module for the proposed hooks; it is not
 * intended to be a polished tool.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "catalog/catalog.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "fmgr.h"
#include "nodes/plannodes.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "portability/instr_time.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_index_overhead",
					.version = "1.0"
);

/* Per-ExplainState options */
typedef struct pioOptions
{
	bool		index_updates;
} pioOptions;

/* Accumulated costs for one index */
typedef struct pioIndexEntry
{
	Oid			indexoid;		/* hash key; must be first */
	Oid			heapoid;		/* table the index belongs to */
	int64		tuples;			/* index_insert() calls */
	instr_time	itime;			/* time inside insert + cleanup */
	WalUsage	wal;
	BufferUsage buf;
} pioIndexEntry;

/* Before-execution snapshot of a target table's transaction stats */
typedef struct pioHeapEntry
{
	Oid			heapoid;
	int64		ins0;
	int64		upd0;
	int64		hot0;
	int64		del0;
} pioHeapEntry;

static int	es_extension_id;
static bool pio_collecting = false;
static bool pio_timing = false;
static HTAB *pio_hash = NULL;
static List *pio_heap_list = NIL;
static bool pio_heap_snapped = false;
static MemoryContext pio_cxt = NULL;

static ExplainOneQuery_hook_type prev_ExplainOneQuery_hook;
static explain_validate_options_hook_type prev_explain_validate_options_hook;
static explain_per_node_hook_type prev_explain_per_node_hook;
static index_insert_hook_type prev_index_insert_hook;
static index_insert_cleanup_hook_type prev_index_insert_cleanup_hook;
static ExecutorStart_hook_type prev_ExecutorStart_hook;

static void pio_index_updates_handler(ExplainState *es, DefElem *opt,
									  ParseState *pstate);
static void pio_validate_options(ExplainState *es, List *options,
								 ParseState *pstate);
static void pio_ExplainOneQuery(Query *query, int cursorOptions,
								IntoClause *into, ExplainState *es,
								const char *queryString, ParamListInfo params,
								QueryEnvironment *queryEnv);
static void pio_ExecutorStart(QueryDesc *queryDesc, int eflags);
static bool pio_index_insert(Relation indexRelation,
							 Datum *values, bool *isnull,
							 ItemPointer heap_t_ctid,
							 Relation heapRelation,
							 IndexUniqueCheck checkUnique,
							 bool indexUnchanged,
							 IndexInfo *indexInfo);
static void pio_index_insert_cleanup(Relation indexRelation,
									 IndexInfo *indexInfo);
static void pio_per_node_hook(PlanState *planstate, List *ancestors,
							  const char *relationship,
							  const char *plan_name, ExplainState *es);

void
_PG_init(void)
{
	es_extension_id = GetExplainExtensionId("pg_index_overhead");

	RegisterExtensionExplainOption("index_updates",
								   pio_index_updates_handler,
								   GUCCheckBooleanExplainOption);

	prev_ExplainOneQuery_hook = ExplainOneQuery_hook;
	ExplainOneQuery_hook = pio_ExplainOneQuery;
	prev_explain_validate_options_hook = explain_validate_options_hook;
	explain_validate_options_hook = pio_validate_options;
	prev_explain_per_node_hook = explain_per_node_hook;
	explain_per_node_hook = pio_per_node_hook;
	prev_index_insert_hook = index_insert_hook;
	index_insert_hook = pio_index_insert;
	prev_index_insert_cleanup_hook = index_insert_cleanup_hook;
	index_insert_cleanup_hook = pio_index_insert_cleanup;
	prev_ExecutorStart_hook = ExecutorStart_hook;
	ExecutorStart_hook = pio_ExecutorStart;
}

/*
 * Fetch or create our options struct in an ExplainState.
 */
static pioOptions *
pio_ensure_options(ExplainState *es)
{
	pioOptions *options;

	options = GetExplainExtensionState(es, es_extension_id);

	if (options == NULL)
	{
		options = palloc0_object(pioOptions);
		SetExplainExtensionState(es, es_extension_id, options);
	}

	return options;
}

/*
 * Parse handler for EXPLAIN (INDEX_UPDATES).
 */
static void
pio_index_updates_handler(ExplainState *es, DefElem *opt, ParseState *pstate)
{
	pioOptions *options = pio_ensure_options(es);

	options->index_updates = defGetBoolean(opt);
}

/*
 * INDEX_UPDATES requires ANALYZE, like WAL does.
 */
static void
pio_validate_options(ExplainState *es, List *options, ParseState *pstate)
{
	pioOptions *opts;

	if (prev_explain_validate_options_hook)
		(*prev_explain_validate_options_hook) (es, options, pstate);

	opts = GetExplainExtensionState(es, es_extension_id);
	if (opts != NULL && opts->index_updates && !es->analyze)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("EXPLAIN option %s requires ANALYZE",
						"INDEX_UPDATES")));
}

/*
 * Reset collection state ahead of an instrumented statement.
 */
static void
pio_reset(void)
{
	HASHCTL		ctl;

	if (pio_cxt == NULL)
		pio_cxt = AllocSetContextCreate(TopMemoryContext,
										"pg_index_overhead",
										ALLOCSET_DEFAULT_SIZES);

	pio_hash = NULL;
	pio_heap_list = NIL;
	pio_heap_snapped = false;
	MemoryContextReset(pio_cxt);

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(pioIndexEntry);
	ctl.hcxt = pio_cxt;
	pio_hash = hash_create("pg_index_overhead entries", 64, &ctl,
						   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * ExplainOneQuery hook: turn collection on around the execution of a
 * statement being explained with (ANALYZE, INDEX_UPDATES).
 */
static void
pio_ExplainOneQuery(Query *query, int cursorOptions, IntoClause *into,
					ExplainState *es, const char *queryString,
					ParamListInfo params, QueryEnvironment *queryEnv)
{
	pioOptions *options = GetExplainExtensionState(es, es_extension_id);
	bool		activate;

	activate = (options != NULL && options->index_updates &&
				es->analyze && !pio_collecting);

	if (activate)
	{
		pio_reset();
		pio_collecting = true;
		pio_timing = es->timing;
	}

	PG_TRY();
	{
		if (prev_ExplainOneQuery_hook)
			(*prev_ExplainOneQuery_hook) (query, cursorOptions, into, es,
										  queryString, params, queryEnv);
		else
			standard_ExplainOneQuery(query, cursorOptions, into, es,
									 queryString, params, queryEnv);
	}
	PG_FINALLY();
	{
		if (activate)
			pio_collecting = false;
	}
	PG_END_TRY();
}

/*
 * ExecutorStart hook: snapshot the transaction-level stats counters of the
 * target relations, so that heap-level modification counts (notably HOT
 * updates) can be computed at reporting time.  Only the outermost
 * statement of an instrumented EXPLAIN is snapshotted.
 */
static void
pio_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (prev_ExecutorStart_hook)
		(*prev_ExecutorStart_hook) (queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (pio_collecting && !pio_heap_snapped &&
		!bms_is_empty(queryDesc->plannedstmt->resultRelationRelids))
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(pio_cxt);
		int			rti = -1;

		while ((rti = bms_next_member(queryDesc->plannedstmt->resultRelationRelids,
									  rti)) >= 0)
		{
			RangeTblEntry *rte = rt_fetch(rti, queryDesc->plannedstmt->rtable);
			pioHeapEntry *h;
			PgStat_TableStatus *ts;

			h = palloc0_object(pioHeapEntry);
			h->heapoid = rte->relid;
			ts = find_tabstat_entry(rte->relid);
			if (ts != NULL)
			{
				h->ins0 = ts->counts.tuples_inserted;
				h->upd0 = ts->counts.tuples_updated;
				h->hot0 = ts->counts.tuples_hot_updated;
				h->del0 = ts->counts.tuples_deleted;
			}
			pio_heap_list = lappend(pio_heap_list, h);
		}
		MemoryContextSwitchTo(oldcxt);
		pio_heap_snapped = true;
	}
}

/*
 * Find or create the accumulator for an index.
 */
static pioIndexEntry *
pio_get_entry(Oid indexoid, Oid heapoid)
{
	pioIndexEntry *entry;
	bool		found;

	entry = hash_search(pio_hash, &indexoid, HASH_ENTER, &found);
	if (!found)
	{
		entry->heapoid = heapoid;
		entry->tuples = 0;
		INSTR_TIME_SET_ZERO(entry->itime);
		memset(&entry->wal, 0, sizeof(WalUsage));
		memset(&entry->buf, 0, sizeof(BufferUsage));
	}
	return entry;
}

/*
 * index_insert hook: bracket the real insertion with time/WAL/buffer
 * accounting when collection is active.
 */
static bool
pio_index_insert(Relation indexRelation, Datum *values, bool *isnull,
				 ItemPointer heap_t_ctid, Relation heapRelation,
				 IndexUniqueCheck checkUnique, bool indexUnchanged,
				 IndexInfo *indexInfo)
{
	instr_time	start;
	instr_time	end;
	WalUsage	walstart;
	BufferUsage bufstart;
	pioIndexEntry *entry;
	bool		ret;

	if (!pio_collecting)
	{
		if (prev_index_insert_hook)
			return (*prev_index_insert_hook) (indexRelation, values, isnull,
											  heap_t_ctid, heapRelation,
											  checkUnique, indexUnchanged,
											  indexInfo);
		return standard_index_insert(indexRelation, values, isnull,
									 heap_t_ctid, heapRelation,
									 checkUnique, indexUnchanged,
									 indexInfo);
	}

	walstart = pgWalUsage;
	bufstart = pgBufferUsage;
	INSTR_TIME_SET_ZERO(start);
	if (pio_timing)
		INSTR_TIME_SET_CURRENT(start);

	if (prev_index_insert_hook)
		ret = (*prev_index_insert_hook) (indexRelation, values, isnull,
										 heap_t_ctid, heapRelation,
										 checkUnique, indexUnchanged,
										 indexInfo);
	else
		ret = standard_index_insert(indexRelation, values, isnull,
									heap_t_ctid, heapRelation,
									checkUnique, indexUnchanged,
									indexInfo);

	entry = pio_get_entry(RelationGetRelid(indexRelation),
						  RelationGetRelid(heapRelation));
	entry->tuples++;
	if (pio_timing)
	{
		INSTR_TIME_SET_CURRENT(end);
		INSTR_TIME_ACCUM_DIFF(entry->itime, end, start);
	}
	WalUsageAccumDiff(&entry->wal, &pgWalUsage, &walstart);
	BufferUsageAccumDiff(&entry->buf, &pgBufferUsage, &bufstart);

	return ret;
}

/*
 * index_insert_cleanup hook: attribute deferred insertion work (e.g. GIN
 * pending-list flushes) to the index as well.
 */
static void
pio_index_insert_cleanup(Relation indexRelation, IndexInfo *indexInfo)
{
	instr_time	start;
	instr_time	end;
	WalUsage	walstart;
	BufferUsage bufstart;
	pioIndexEntry *entry;

	if (!pio_collecting)
	{
		if (prev_index_insert_cleanup_hook)
			(*prev_index_insert_cleanup_hook) (indexRelation, indexInfo);
		else
			standard_index_insert_cleanup(indexRelation, indexInfo);
		return;
	}

	walstart = pgWalUsage;
	bufstart = pgBufferUsage;
	INSTR_TIME_SET_ZERO(start);
	if (pio_timing)
		INSTR_TIME_SET_CURRENT(start);

	if (prev_index_insert_cleanup_hook)
		(*prev_index_insert_cleanup_hook) (indexRelation, indexInfo);
	else
		standard_index_insert_cleanup(indexRelation, indexInfo);

	entry = pio_get_entry(RelationGetRelid(indexRelation),
						  indexRelation->rd_index->indrelid);
	if (pio_timing)
	{
		INSTR_TIME_SET_CURRENT(end);
		INSTR_TIME_ACCUM_DIFF(entry->itime, end, start);
	}
	WalUsageAccumDiff(&entry->wal, &pgWalUsage, &walstart);
	BufferUsageAccumDiff(&entry->buf, &pgBufferUsage, &bufstart);
}

/*
 * Sort helper: most expensive index first.
 */
static int
pio_entry_cmp(const void *a, const void *b)
{
	const pioIndexEntry *ea = *(const pioIndexEntry *const *) a;
	const pioIndexEntry *eb = *(const pioIndexEntry *const *) b;
	double		ta = INSTR_TIME_GET_DOUBLE(ea->itime);
	double		tb = INSTR_TIME_GET_DOUBLE(eb->itime);

	if (ta != tb)
		return (ta < tb) ? 1 : -1;
	if (ea->tuples != eb->tuples)
		return (ea->tuples < eb->tuples) ? 1 : -1;
	if (ea->indexoid != eb->indexoid)
		return (ea->indexoid < eb->indexoid) ? -1 : 1;
	return 0;
}

/*
 * Emit the "Heap ..." summary lines for one ModifyTable node.
 */
static void
pio_show_heap(List *relids, ExplainState *es)
{
	int64		ins = 0;
	int64		upd = 0;
	int64		hot = 0;
	int64		del = 0;
	bool		have = false;
	ListCell   *lc;

	foreach(lc, pio_heap_list)
	{
		pioHeapEntry *h = (pioHeapEntry *) lfirst(lc);
		PgStat_TableStatus *ts;

		if (!list_member_oid(relids, h->heapoid))
			continue;
		have = true;
		ts = find_tabstat_entry(h->heapoid);
		if (ts != NULL)
		{
			ins += ts->counts.tuples_inserted - h->ins0;
			upd += ts->counts.tuples_updated - h->upd0;
			hot += ts->counts.tuples_hot_updated - h->hot0;
			del += ts->counts.tuples_deleted - h->del0;
		}
	}

	if (!have)
		return;

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		if (ins > 0)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "Heap Inserts: %lld\n",
							 (long long) ins);
		}
		if (upd > 0)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str,
							 "Heap Updates: total=%lld hot=%lld non-hot=%lld\n",
							 (long long) upd, (long long) hot,
							 (long long) (upd - hot));
		}
		if (del > 0)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "Heap Deletes: %lld\n",
							 (long long) del);
		}
	}
	else
	{
		ExplainPropertyInteger("Heap Tuples Inserted", NULL, ins, es);
		ExplainPropertyInteger("Heap Tuples Updated", NULL, upd, es);
		ExplainPropertyInteger("Heap Tuples Hot Updated", NULL, hot, es);
		ExplainPropertyInteger("Heap Tuples Deleted", NULL, del, es);
	}
}

/*
 * Emit the per-index lines for one ModifyTable node.
 */
static void
pio_show_indexes(pioIndexEntry **entries, int nentries, ExplainState *es)
{
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		ExplainIndentText(es);
		appendStringInfoString(es->str, "Index Updates:\n");
		es->indent++;
	}
	else
		ExplainOpenGroup("Index Updates", "Index Updates", false, es);

	for (int i = 0; i < nentries; i++)
	{
		pioIndexEntry *e = entries[i];
		char	   *name = get_rel_name(e->indexoid);

		if (name == NULL)
			name = "(unknown)";

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			ExplainIndentText(es);
			if (es->verbose)
			{
				char	   *nsp =
					get_namespace_name(get_rel_namespace(e->indexoid));

				appendStringInfo(es->str, "%s.%s", nsp ? nsp : "?", name);
			}
			else
				appendStringInfoString(es->str, name);
			appendStringInfo(es->str, ": tuples=%lld", (long long) e->tuples);
			if (es->timing)
				appendStringInfo(es->str, " time=%.3f ms",
								 INSTR_TIME_GET_MILLISEC(e->itime));
			appendStringInfoChar(es->str, '\n');

			if (es->wal &&
				(e->wal.wal_records > 0 || e->wal.wal_fpi > 0 ||
				 e->wal.wal_bytes > 0))
			{
				es->indent++;
				ExplainIndentText(es);
				appendStringInfoString(es->str, "WAL:");
				if (e->wal.wal_records > 0)
					appendStringInfo(es->str, " records=%lld",
									 (long long) e->wal.wal_records);
				if (e->wal.wal_fpi > 0)
					appendStringInfo(es->str, " fpi=%lld",
									 (long long) e->wal.wal_fpi);
				if (e->wal.wal_bytes > 0)
					appendStringInfo(es->str, " bytes=%llu",
									 (unsigned long long) e->wal.wal_bytes);
				appendStringInfoChar(es->str, '\n');
				es->indent--;
			}

			if (es->buffers &&
				(e->buf.shared_blks_hit > 0 || e->buf.shared_blks_read > 0 ||
				 e->buf.shared_blks_dirtied > 0 ||
				 e->buf.shared_blks_written > 0))
			{
				es->indent++;
				ExplainIndentText(es);
				appendStringInfoString(es->str, "Buffers: shared");
				if (e->buf.shared_blks_hit > 0)
					appendStringInfo(es->str, " hit=%lld",
									 (long long) e->buf.shared_blks_hit);
				if (e->buf.shared_blks_read > 0)
					appendStringInfo(es->str, " read=%lld",
									 (long long) e->buf.shared_blks_read);
				if (e->buf.shared_blks_dirtied > 0)
					appendStringInfo(es->str, " dirtied=%lld",
									 (long long) e->buf.shared_blks_dirtied);
				if (e->buf.shared_blks_written > 0)
					appendStringInfo(es->str, " written=%lld",
									 (long long) e->buf.shared_blks_written);
				appendStringInfoChar(es->str, '\n');
				es->indent--;
			}
		}
		else
		{
			ExplainOpenGroup("Index Update", NULL, true, es);
			ExplainPropertyText("Index Name", name, es);
			ExplainPropertyInteger("Tuples", NULL, e->tuples, es);
			if (es->timing)
				ExplainPropertyFloat("Time", "ms",
									 INSTR_TIME_GET_MILLISEC(e->itime),
									 3, es);
			if (es->wal)
			{
				ExplainPropertyInteger("WAL Records", NULL,
									   e->wal.wal_records, es);
				ExplainPropertyInteger("WAL FPI", NULL, e->wal.wal_fpi, es);
				ExplainPropertyUInteger("WAL Bytes", NULL,
										e->wal.wal_bytes, es);
			}
			if (es->buffers)
			{
				ExplainPropertyInteger("Shared Hit Blocks", NULL,
									   e->buf.shared_blks_hit, es);
				ExplainPropertyInteger("Shared Read Blocks", NULL,
									   e->buf.shared_blks_read, es);
				ExplainPropertyInteger("Shared Dirtied Blocks", NULL,
									   e->buf.shared_blks_dirtied, es);
				ExplainPropertyInteger("Shared Written Blocks", NULL,
									   e->buf.shared_blks_written, es);
			}
			ExplainCloseGroup("Index Update", NULL, true, es);
		}
	}

	if (es->format == EXPLAIN_FORMAT_TEXT)
		es->indent--;
	else
		ExplainCloseGroup("Index Updates", "Index Updates", false, es);
}

/*
 * Per-node hook: annotate ModifyTable nodes.
 *
 * Entries are attached to the node whose result relations own their index.
 * Anything left over (TOAST indexes, indexes on partitions the tuple router
 * created result relations for at runtime, indexes updated by trigger-fired
 * statements) is attached to the top node of the plan, except for catalog
 * indexes, which are suppressed.
 */
static void
pio_per_node_hook(PlanState *planstate, List *ancestors,
				  const char *relationship, const char *plan_name,
				  ExplainState *es)
{
	pioOptions *options;
	ModifyTable *mt;
	List	   *relids = NIL;
	ListCell   *lc;
	pioIndexEntry **entries;
	pioIndexEntry *entry;
	int			nentries = 0;
	bool		is_top;
	HASH_SEQ_STATUS seq;

	if (prev_explain_per_node_hook)
		(*prev_explain_per_node_hook) (planstate, ancestors, relationship,
									   plan_name, es);

	options = GetExplainExtensionState(es, es_extension_id);
	if (options == NULL || !options->index_updates || !es->analyze)
		return;
	if (pio_hash == NULL)
		return;
	if (!IsA(planstate->plan, ModifyTable))
		return;

	mt = (ModifyTable *) planstate->plan;
	is_top = (relationship == NULL);

	foreach(lc, mt->resultRelations)
	{
		Index		rti = lfirst_int(lc);
		RangeTblEntry *rte = rt_fetch(rti, es->rtable);

		relids = lappend_oid(relids, rte->relid);
	}

	pio_show_heap(relids, es);

	entries = palloc(hash_get_num_entries(pio_hash) * sizeof(pioIndexEntry *));
	hash_seq_init(&seq, pio_hash);
	while ((entry = hash_seq_search(&seq)) != NULL)
	{
		if (list_member_oid(relids, entry->heapoid))
			entries[nentries++] = entry;
		else if (is_top && !IsCatalogRelationOid(entry->heapoid))
			entries[nentries++] = entry;
	}

	if (nentries > 0)
	{
		qsort(entries, nentries, sizeof(pioIndexEntry *), pio_entry_cmp);
		pio_show_indexes(entries, nentries, es);
	}

	pfree(entries);
	list_free(relids);
}
