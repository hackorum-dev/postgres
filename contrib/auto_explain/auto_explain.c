/*-------------------------------------------------------------------------
 *
 * auto_explain.c
 *
 *
 * Copyright (c) 2008-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/auto_explain/auto_explain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "access/parallel.h"
#include "commands/explain.h"
#include "commands/defrem.h"
#include "executor/instrument.h"
#include "jit/jit.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/planmain.h"
#include "parser/parsetree.h"
#include "storage/ipc.h"
#include "statistics/statistics.h"
#include "utils/guc.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"

PG_MODULE_MAGIC;

/* GUC variables */
static int	auto_explain_log_min_duration = -1; /* msec or -1 */
static bool auto_explain_log_analyze = false;
static bool auto_explain_log_verbose = false;
static bool auto_explain_log_buffers = false;
static bool auto_explain_log_triggers = false;
static bool auto_explain_log_timing = true;
static bool auto_explain_log_settings = false;
static int	auto_explain_log_format = EXPLAIN_FORMAT_TEXT;
static int	auto_explain_log_level = LOG;
static bool auto_explain_log_nested_statements = false;
static double auto_explain_sample_rate = 1;
static double auto_explain_add_statistics_threshold = 0.0;
static double auto_explain_aqo_threshold = 0.0;
static int  auto_explain_aqo_limit = 0;
static bool auto_explain_aqo_disregard_constants = false;
static char* auto_explain_aqo_file;

static void AQO_set_joinrel_size_estimates(PlannerInfo *root, RelOptInfo *rel,
										   RelOptInfo *outer_rel,
										   RelOptInfo *inner_rel,
										   SpecialJoinInfo *sjinfo,
										   List *restrict_clauses);
static void AQO_set_baserel_rows_estimate(PlannerInfo *root, RelOptInfo *rel);
static double AQO_get_parameterized_joinrel_size(PlannerInfo *root, RelOptInfo *rel,
											 Path *outer_path,
											 Path *inner_path,
											 SpecialJoinInfo *sjinfo,
											 List *restrict_clauses);
static double AQO_get_parameterized_baserel_size(PlannerInfo *root, RelOptInfo *rel,
												 List *param_clauses);
static void AQO_copy_generic_path_info(Plan *dest, Path *src);
static void LoadAQOHash(char const* file);
static void SaveAQOHash(char const* file);


static const struct config_enum_entry format_options[] = {
	{"text", EXPLAIN_FORMAT_TEXT, false},
	{"xml", EXPLAIN_FORMAT_XML, false},
	{"json", EXPLAIN_FORMAT_JSON, false},
	{"yaml", EXPLAIN_FORMAT_YAML, false},
	{NULL, 0, false}
};

static const struct config_enum_entry loglevel_options[] = {
	{"debug5", DEBUG5, false},
	{"debug4", DEBUG4, false},
	{"debug3", DEBUG3, false},
	{"debug2", DEBUG2, false},
	{"debug1", DEBUG1, false},
	{"debug", DEBUG2, true},
	{"info", INFO, false},
	{"notice", NOTICE, false},
	{"warning", WARNING, false},
	{"log", LOG, false},
	{NULL, 0, false}
};

/* Current nesting depth of ExecutorRun calls */
static int	nesting_level = 0;

/* Is the current top-level query to be sampled? */
static bool current_query_sampled = false;

#define auto_explain_enabled() \
	(auto_explain_log_min_duration >= 0 && \
	 (nesting_level == 0 || auto_explain_log_nested_statements) && \
	 current_query_sampled)

/* Saved hook values in case of unload */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static set_joinrel_size_estimates_hook_type prev_set_joinrel_size_estimates_hook;
static set_baserel_rows_estimate_hook_type prev_set_baserel_rows_estimate_hook;
static get_parameterized_joinrel_size_hook_type prev_get_parameterized_joinrel_size_hook;
static get_parameterized_baserel_size_hook_type prev_get_parameterized_baserel_size_hook;
static copy_generic_path_info_hook_type prev_copy_generic_path_info_hook;

void		_PG_init(void);
void		_PG_fini(void);

static void explain_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void explain_ExecutorRun(QueryDesc *queryDesc,
								ScanDirection direction,
								uint64 count, bool execute_once);
static void explain_ExecutorFinish(QueryDesc *queryDesc);
static void explain_ExecutorEnd(QueryDesc *queryDesc);


/*
 * Module load callback
 */
void
_PG_init(void)
{
	/* Define custom GUC variables. */
	DefineCustomIntVariable("auto_explain.log_min_duration",
							"Sets the minimum execution time above which plans will be logged.",
							"Zero prints all plans. -1 turns this feature off.",
							&auto_explain_log_min_duration,
							-1,
							-1, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_MS,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("auto_explain.log_analyze",
							 "Use EXPLAIN ANALYZE for plan logging.",
							 NULL,
							 &auto_explain_log_analyze,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain.log_settings",
							 "Log modified configuration parameters affecting query planning.",
							 NULL,
							 &auto_explain_log_settings,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain.log_verbose",
							 "Use EXPLAIN VERBOSE for plan logging.",
							 NULL,
							 &auto_explain_log_verbose,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain.log_buffers",
							 "Log buffers usage.",
							 NULL,
							 &auto_explain_log_buffers,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain.log_triggers",
							 "Include trigger statistics in plans.",
							 "This has no effect unless log_analyze is also set.",
							 &auto_explain_log_triggers,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomEnumVariable("auto_explain.log_format",
							 "EXPLAIN format to be used for plan logging.",
							 NULL,
							 &auto_explain_log_format,
							 EXPLAIN_FORMAT_TEXT,
							 format_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomEnumVariable("auto_explain.log_level",
							 "Log level for the plan.",
							 NULL,
							 &auto_explain_log_level,
							 LOG,
							 loglevel_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain.log_nested_statements",
							 "Log nested statements.",
							 NULL,
							 &auto_explain_log_nested_statements,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain.log_timing",
							 "Collect timing data, not just row counts.",
							 NULL,
							 &auto_explain_log_timing,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomRealVariable("auto_explain.sample_rate",
							 "Fraction of queries to process.",
							 NULL,
							 &auto_explain_sample_rate,
							 1.0,
							 0.0,
							 1.0,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomRealVariable("auto_explain.add_statistics_threshold",
							 "Sets the threshold for actual/estimated #rows ratio triggering creation of multicolumn statistic for the related columns.",
							 "Zero disables implicit creation of multicolumn statistic.",
							 &auto_explain_add_statistics_threshold,
							 0.0,
							 0.0,
							 INT_MAX,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomRealVariable("auto_explain.aqo_threshold",
							 "Sets the threshold for actual/estimated #rows ratio for storing AQO correction coefficient for plan nodes",
							 "Non-zero value of this parameter enables AQO learning mode when it collects information and uses it for plan adjustment. "
							 "Negative value of this parameter disable update of AQO statistic but enables using it for query adjustment. "
							 "Zero value completely disables AQO",
							 &auto_explain_aqo_threshold,
							 0.0,
							 -1,
							 INT_MAX,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomStringVariable("auto_explain.aqo_file",
							   "File for saving/loading AQO information",
							   NULL,
							   &auto_explain_aqo_file,
							   "",
							   PGC_SUSET,
							   0,
							   NULL,
							   NULL,
							   NULL);
	DefineCustomIntVariable("auto_explain.aqo_limit",
							"Limit number of clauses for which AQO information is stored.",
							"Non-zero value of this parameter limits about of information stored by AQO (to avoid memory overflow). "
							"LRU replacement algorithm is used to maintain AQO hash.",
							 &auto_explain_aqo_limit,
							 0,
							 0,
							 INT_MAX,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain.aqo_disregard_constants",
							 "Do not take in account constant values when matching plans.",
							 NULL,
							 &auto_explain_aqo_disregard_constants,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	EmitWarningsOnPlaceholders("auto_explain");

	/* Install hooks. */
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = explain_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = explain_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = explain_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = explain_ExecutorEnd;

	prev_set_baserel_rows_estimate_hook = set_baserel_rows_estimate_hook;
	set_baserel_rows_estimate_hook = AQO_set_baserel_rows_estimate;

	prev_set_joinrel_size_estimates_hook = set_joinrel_size_estimates_hook;
	set_joinrel_size_estimates_hook = AQO_set_joinrel_size_estimates;

	prev_get_parameterized_joinrel_size_hook = get_parameterized_joinrel_size_hook;
	get_parameterized_joinrel_size_hook = AQO_get_parameterized_joinrel_size;

	prev_get_parameterized_baserel_size_hook = get_parameterized_baserel_size_hook;
	get_parameterized_baserel_size_hook = AQO_get_parameterized_baserel_size;

	prev_copy_generic_path_info_hook = copy_generic_path_info_hook;
	copy_generic_path_info_hook = AQO_copy_generic_path_info;
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	/* Uninstall hooks. */
	ExecutorStart_hook = prev_ExecutorStart;
	ExecutorRun_hook = prev_ExecutorRun;
	ExecutorFinish_hook = prev_ExecutorFinish;
	ExecutorEnd_hook = prev_ExecutorEnd;
	set_joinrel_size_estimates_hook = prev_set_joinrel_size_estimates_hook;
	set_baserel_rows_estimate_hook = prev_set_baserel_rows_estimate_hook;
	get_parameterized_joinrel_size_hook = prev_get_parameterized_joinrel_size_hook;
	get_parameterized_baserel_size_hook = prev_get_parameterized_baserel_size_hook;
	copy_generic_path_info_hook = prev_copy_generic_path_info_hook;
}

/*
 * ExecutorStart hook: start up logging if needed
 */
static void
explain_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	/*
	 * At the beginning of each top-level statement, decide whether we'll
	 * sample this statement.  If nested-statement explaining is enabled,
	 * either all nested statements will be explained or none will.
	 *
	 * When in a parallel worker, we should do nothing, which we can implement
	 * cheaply by pretending we decided not to sample the current statement.
	 * If EXPLAIN is active in the parent session, data will be collected and
	 * reported back to the parent, and it's no business of ours to interfere.
	 */
	if (nesting_level == 0)
	{
		if (auto_explain_log_min_duration >= 0 && !IsParallelWorker())
			current_query_sampled = (random() < auto_explain_sample_rate *
									 ((double) MAX_RANDOM_VALUE + 1));
		else
			current_query_sampled = false;
	}

	if (auto_explain_enabled())
	{
		/* Enable per-node instrumentation iff log_analyze is required. */
		if (auto_explain_log_analyze && (eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
		{
			if (auto_explain_log_timing)
				queryDesc->instrument_options |= INSTRUMENT_TIMER;
			else
				queryDesc->instrument_options |= INSTRUMENT_ROWS;
			if (auto_explain_log_buffers)
				queryDesc->instrument_options |= INSTRUMENT_BUFFERS;
		}
	}

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (auto_explain_enabled())
	{
		/*
		 * Set up to track total elapsed time in ExecutorRun.  Make sure the
		 * space is allocated in the per-query context so it will go away at
		 * ExecutorEnd.
		 */
		if (queryDesc->totaltime == NULL)
		{
			MemoryContext oldcxt;

			oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
			queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL);
			MemoryContextSwitchTo(oldcxt);
		}
	}
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
explain_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
					uint64 count, bool execute_once)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
		nesting_level--;
	}
	PG_CATCH();
	{
		nesting_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
explain_ExecutorFinish(QueryDesc *queryDesc)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
		nesting_level--;
	}
	PG_CATCH();
	{
		nesting_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

static void
AddMultiColumnStatisticsForNode(PlanState *planstate, ExplainState *es);

static void
AddMultiColumnStatisticsForSubPlans(List *plans, ExplainState *es)
{
	ListCell   *lst;

	foreach(lst, plans)
	{
		SubPlanState *sps = (SubPlanState *) lfirst(lst);

		AddMultiColumnStatisticsForNode(sps->planstate, es);
	}
}

static void
AddMultiColumnStatisticsForMemberNodes(PlanState **planstates, int nsubnodes,
									ExplainState *es)
{
	int			j;

	for (j = 0; j < nsubnodes; j++)
		AddMultiColumnStatisticsForNode(planstates[j], es);
}

static void
AddMultiColumnStatisticsForQual(void* qual, ExplainState *es)
{
	List *vars = NULL;
	ListCell* lc;
	foreach (lc, qual)
	{
		Node* node = (Node*)lfirst(lc);
		if (IsA(node, RestrictInfo))
			node = (Node*)((RestrictInfo*)node)->clause;
		vars = list_concat(vars, pull_vars_of_level(node, 0));
	}
	while (vars != NULL)
	{
		ListCell *cell, *next, *prev = NULL;
		List *cols = NULL;
		Index varno = 0;
		Bitmapset* colmap = NULL;

		for (cell = list_head(vars); cell != NULL; cell = next)
		{
			Node* node = (Node *) lfirst(cell);
			next = lnext(cell);
			if (IsA(node, Var))
			{
				Var *var = (Var *) node;
				if (cols == NULL || var->varno == varno)
				{
					varno = var->varno;
					if (var->varattno > 0 &&
						!bms_is_member(var->varattno, colmap) &&
						varno >= 1 &&
						varno <= list_length(es->rtable) &&
						list_length(cols) < STATS_MAX_DIMENSIONS)
					{
						RangeTblEntry *rte = rt_fetch(varno, es->rtable);
						if (rte->rtekind == RTE_RELATION)
						{
							ColumnRef  *col = makeNode(ColumnRef);
							char *colname = get_rte_attribute_name(rte, var->varattno);
							col->fields = list_make1(makeString(colname));
							cols = lappend(cols, col);
							colmap = bms_add_member(colmap, var->varattno);
						}
					}
				}
				else
				{
					prev = cell;
					continue;
				}
			}
			vars = list_delete_cell(vars, cell, prev);
		}
		if (list_length(cols) >= 2)
		{
			CreateStatsStmt* stats = makeNode(CreateStatsStmt);
			RangeTblEntry *rte = rt_fetch(varno, es->rtable);
			char *rel_namespace = get_namespace_name(get_rel_namespace(rte->relid));
			char *rel_name = get_rel_name(rte->relid);
			RangeVar* rel = makeRangeVar(rel_namespace, rel_name, 0);
			char* stat_name = rel_name;

			/* Construct name for statistic by concatenating relation name with all columns */
			foreach (cell, cols)
				stat_name = psprintf("%s_%s", stat_name, strVal((Value *) linitial(((ColumnRef *)lfirst(cell))->fields)));

			/*
			 * Check if multicolumn if multicolumn statistic object with such name already exists
			 * (most likely if was already created by auto_explain, but either ANALYZE was not performed since
			 * this time, either presence of this multicolumn statistic doesn't help to provide more precise estimation.
			 * Despite to the fact that we create statistics with "if_not_exist" option, presence of such check
			 * allows to eliminate notice message that statistics object already exists.
			 */
			if (!SearchSysCacheExists2(STATEXTNAMENSP,
									   CStringGetDatum(stat_name),
									   ObjectIdGetDatum(get_rel_namespace(rte->relid))))
			{
				stats->defnames = list_make2(makeString(rel_namespace), makeString(stat_name));
				stats->if_not_exists = true;
				stats->relations = list_make1(rel);
				stats->exprs = cols;
				CreateStatistics(stats);
			}
		}
	}
}

static void
AddMultiColumnStatisticsForNode(PlanState *planstate, ExplainState *es)
{
	Plan	   *plan = planstate->plan;

	if (planstate->instrument && plan->plan_rows != 0)
	{
		if (auto_explain_add_statistics_threshold != 0
			&& planstate->instrument->ntuples / plan->plan_rows >= auto_explain_add_statistics_threshold)
		{
			AddMultiColumnStatisticsForQual(plan->path_clauses, es);
		}
	}

	/* initPlan-s */
	if (planstate->initPlan)
		AddMultiColumnStatisticsForSubPlans(planstate->initPlan, es);

	/* lefttree */
	if (outerPlanState(planstate))
		AddMultiColumnStatisticsForNode(outerPlanState(planstate), es);

	/* righttree */
	if (innerPlanState(planstate))
		AddMultiColumnStatisticsForNode(innerPlanState(planstate), es);

	/* special child plans */
	switch (nodeTag(plan))
	{
		case T_ModifyTable:
			AddMultiColumnStatisticsForMemberNodes(((ModifyTableState *) planstate)->mt_plans,
												   ((ModifyTableState *) planstate)->mt_nplans,
												   es);
			break;
		case T_Append:
			AddMultiColumnStatisticsForMemberNodes(((AppendState *) planstate)->appendplans,
												   ((AppendState *) planstate)->as_nplans,
												   es);
			break;
		case T_MergeAppend:
			AddMultiColumnStatisticsForMemberNodes(((MergeAppendState *) planstate)->mergeplans,
												   ((MergeAppendState *) planstate)->ms_nplans,
												   es);
			break;
		case T_BitmapAnd:
			AddMultiColumnStatisticsForMemberNodes(((BitmapAndState *) planstate)->bitmapplans,
												   ((BitmapAndState *) planstate)->nplans,
												   es);
			break;
		case T_BitmapOr:
			AddMultiColumnStatisticsForMemberNodes(((BitmapOrState *) planstate)->bitmapplans,
												   ((BitmapOrState *) planstate)->nplans,
												   es);
			break;
		case T_SubqueryScan:
			AddMultiColumnStatisticsForNode(((SubqueryScanState *) planstate)->subplan, es);
			break;
		default:
			break;
	}
}

/*
 * Adaptive query optimization
 */
#ifndef AQO_DEBUG
#define AQO_DEBUG 0
#endif

#if AQO_DEBUG
#define AQO_LOG LOG
#define AQO_PRETTY_KEY(key) key
#else
#define AQO_LOG DEBUG1
#define AQO_PRETTY_KEY(key) ""
#endif

static void
StoreAQOInfoForNode(PlanState *planstate, ExplainState *es);

static void
StoreAQOInfoForSubPlans(List *plans, ExplainState *es)
{
	ListCell   *lst;

	foreach(lst, plans)
	{
		SubPlanState *sps = (SubPlanState *) lfirst(lst);

		StoreAQOInfoForNode(sps->planstate, es);
	}
}

static void
StoreAQOInfoForMemberNodes(PlanState **planstates, int nsubnodes,
						   ExplainState *es)
{
	int			j;

	for (j = 0; j < nsubnodes; j++)
		StoreAQOInfoForNode(planstates[j], es);
}

/*
 * Replace location in plan nodes with -1 to make it possible to match nodes from different queries.
 */
static Node*
ReplaceLocation(Node* expr, int location)
{
	if (expr == NULL)
		return NULL;
	switch (nodeTag(expr))
	{
		case T_RangeVar:
			((RangeVar *) expr)->location = location;
			break;
		case T_TableFunc:
			((TableFunc *) expr)->location = location;
			break;
		case T_Var:
		    ((Var *) expr)->location = location;
			break;
		case T_Const:
		    ((Const *) expr)->location = location;
			break;
		case T_Param:
			((Param *) expr)->location = location;
			break;
		case T_OpExpr:
		case T_NullIfExpr:		/* struct-equivalent to OpExpr */
		    ((OpExpr *) expr)->location = location;
			break;
		case T_ScalarArrayOpExpr:
		    ((ScalarArrayOpExpr *) expr)->location = location;
			break;
		case T_BoolExpr:
		  ((BoolExpr *) expr)->location = location;
			break;
		case T_RelabelType:
			((RelabelType *) expr)->location = location;
			break;
		case T_CaseExpr:
			((CaseExpr *) expr)->location = location;
			break;
		case T_CaseWhen:
		    ((CaseWhen *) expr)->location = location;
			break;
		case T_ArrayExpr:
			((ArrayExpr *) expr)->location = location;
			break;
		case T_NullTest:
			((NullTest *) expr)->location = location;
			break;
		case T_BooleanTest:
			((BooleanTest *) expr)->location = location;
			break;
		case T_CoerceToDomain:
			((CoerceToDomain *) expr)->location = location;
			break;
		case T_CoerceToDomainValue:
			((CoerceToDomainValue *) expr)->location = location;
			break;
		case T_ColumnRef:
			((ColumnRef *) expr)->location = location;
			break;
		case T_ParamRef:
			((ParamRef *) expr)->location = location;
			break;
		case T_A_Const:
			((A_Const*) expr)->location = location;
			break;
		case T_A_Expr:
			((A_Expr*)expr)->location = location;
			break;
		case T_FuncCall:
			((FuncCall *) expr)->location = location;
			break;
		case T_A_ArrayExpr:
			((ArrayExpr *) expr)->location = location;
			break;
		case T_TypeCast:
			((TypeCast *) expr)->location = location;
			break;
		default:
			break;
	 }
	return expr;
}

/*
 * Clause trasformation required to use expression tree as hash key.
 * We do two unconditional tranformation:
 * 1. Replace varno with OID in Var nodes
 * 2. Replace location with -1
 * and one conditional transformation of constant to parameters when auto_explain_aqo_disregard_constants is on.
 */
static Node*
TransformClause(Node* node, void* context)
{
	if (node == NULL)
		return NULL;
	if (auto_explain_aqo_disregard_constants && IsA(node, Const))
	{
		Const* constant = (Const*)node;
		Param* param = makeNode(Param);
		param->paramkind = PARAM_EXEC;
		param->paramtype = constant->consttype;
		param->paramtypmod = constant->consttypmod;
		param->paramcollid = constant->constcollid;
		param->location = -1;
		return (Node*)param;
	}
	else if (IsA(node, Var))
	{
		Var* oldvar = (Var*)node;
		if (oldvar->varno > 0)
		{
			List* rtable = (List*)context;
			Var* newvar = makeNode(Var);
			RangeTblEntry *rte = rt_fetch(oldvar->varno, rtable);
			*newvar = *oldvar;
			newvar->varno = newvar->varnoold = rte->relid;
			newvar->location = -1;
			return (Node*)newvar;
		}
	}
	return ReplaceLocation(expression_tree_mutator(node, TransformClause, context), -1);
}

/*
 * Convert quals list to string used as hash key.
 * expression_tree_mutator is not able to handle ReistrictClause nodes, so we have to perform loop here.
 */
static char*
QualsListToString(List* quals, List* rtable)
{
	ListCell *lc;
	List* clauses = NULL;
	foreach(lc, quals)
	{
		Node* node = (Node*)lfirst(lc);
		if (IsA(node, RestrictInfo))
			node = (Node*)((RestrictInfo*)node)->clause;
		node = TransformClause(node, (void*)rtable);
		clauses = lappend(clauses, node);
	}
	return nodeToString(clauses);
}

typedef struct AQOHashKey
{
	char*  predicate; /* text represantation of path clause */
} AQOHashKey;

/*
 * Number of bins (correction coeficients) stored for each hash entry.
 * Each bin corresponds to log10 of estimated nubmer fo rows.
 */
#define AQO_MAX_BINS 6

#define MAX_AQO_KEY_LEN (16*1024)

typedef struct AQOHashEntry
{
	AQOHashKey           key;
	float                correction[AQO_MAX_BINS];
	struct AQOHashEntry* next; /* LRU list */
	struct AQOHashEntry* prev;
} AQOHashEntry;

static uint32
AQOEntryHashFunc(const void *key, Size keysize)
{
	AQOHashKey* entry = (AQOHashKey*)key;
	return string_hash(entry->predicate, INT_MAX);
}

static int
AQOEntryMatchFunc(const void *key1, const void *key2, Size keysize)
{
	AQOHashKey* e1 = (AQOHashKey*)key1;
	AQOHashKey* e2 = (AQOHashKey*)key2;
    return strcmp(e1->predicate, e2->predicate);
}

static void*
AQOEntryCopyFunc(void *dst_entry, const void *src_entry, Size keysize)
{
	AQOHashKey* dst = (AQOHashKey*)dst_entry;
	AQOHashKey* src = (AQOHashKey*)src_entry;
	dst->predicate = MemoryContextStrdup(TopMemoryContext, src->predicate);
	return dst;
}

static HTAB* AQOHash;
#define AQO_HASH_INIT_SIZE 1013  /* start small and extend */

static AQOHashEntry AQOHashLRU;
static bool AQOHashUpdated;

static void
AQO_on_exit_callback(int code, Datum arg)
{
	if (auto_explain_aqo_file && *auto_explain_aqo_file && AQOHashUpdated)
		SaveAQOHash(auto_explain_aqo_file);
}

static void
InitAQOHash(void)
{
	if (AQOHash == NULL)
	{
		HASHCTL		hash_ctl;

		/* Create the hashtable proper */
		MemSet(&hash_ctl, 0, sizeof(hash_ctl));
		hash_ctl.keysize = sizeof(AQOHashKey);
		hash_ctl.entrysize = sizeof(AQOHashEntry);
		hash_ctl.hash = AQOEntryHashFunc;
		hash_ctl.match = AQOEntryMatchFunc;
		hash_ctl.keycopy = AQOEntryCopyFunc;
		hash_ctl.hcxt = TopMemoryContext;
		AQOHash = hash_create("aqo_hash",
							  auto_explain_aqo_limit != 0 ? auto_explain_aqo_limit : AQO_HASH_INIT_SIZE,
							  &hash_ctl,
							  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_KEYCOPY | HASH_CONTEXT);
		AQOHashLRU.next = AQOHashLRU.prev = &AQOHashLRU;

		on_proc_exit(AQO_on_exit_callback, 0);
	}
}

static void SaveAQOHash(char const* file)
{
	char tmp_file[MAXPGPATH];
	int fd;
	FILE* f;
	sprintf(tmp_file, "%s.XXXXXX", file);
	fd = mkstemp(tmp_file);
	f = fdopen(fd, "w");
	if (f == NULL)
	{
		elog(WARNING, "Failed to save AQO information in file %s: %m", tmp_file);
		return;
	}
	if (AQOHash)
	{
		HASH_SEQ_STATUS status;
        AQOHashEntry* entry;
		int i;
		hash_seq_init(&status, AQOHash);

        while ((entry = hash_seq_search(&status)) != NULL)
		{
			char sep = '\n';
			fputs(entry->key.predicate, f);
			for (i = 0; i < AQO_MAX_BINS; i++)
			{
				fputc(sep, f);
				fprintf(f, "%f", entry->correction[i]);
				sep = ' ';
			}
			fputc('\n', f);
		}
	}
	if (fclose(f) < 0)
		elog(WARNING, "Failed to write AQO file %s: %m", tmp_file);
	if (rename(tmp_file, file) < 0)
		elog(WARNING, "Failed to save AQO file %s: %m", file);
}

static void LoadAQOHash(char const* file)
{
	FILE* f = fopen(file, "r");
	char buf[MAX_AQO_KEY_LEN];
	AQOHashKey key;
	key.predicate = buf;
	if (f == NULL)
	{
		elog(WARNING, "Failed to load AQO information from file %s: %m", file);
		return;
	}
	InitAQOHash();
	while (fgets(buf, sizeof buf, f))
	{
		AQOHashEntry* entry;
		int i;
		buf[strlen(buf)-1] = '\0'; /* remove new line */
		entry = hash_search(AQOHash, &key, HASH_ENTER, NULL);
		for (i = 0; i < AQO_MAX_BINS; i++)
			fscanf(f, "%f", &entry->correction[i]);
		fgets(buf, sizeof buf, f); /* read end of line */
	}
}

static bool HasAQOData(void)
{
	if (!AQOHash && auto_explain_aqo_threshold
		&& auto_explain_aqo_file && *auto_explain_aqo_file)
	{
		LoadAQOHash(auto_explain_aqo_file);
	}
	return AQOHash != NULL;
}

static void
LRUListUnlink(AQOHashEntry* entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
}

static void
LRUListInsert(AQOHashEntry* list, AQOHashEntry* entry)
{
	entry->next = list->next;
	entry->prev = list;
	list->next = list->next->prev = entry;
}

#if AQO_DEBUG
/*
 * Print user-friendly representation of quals list. Used only for debugging purposes.
 */
static char*
PrintPlanNode(PlanState *planstate, List* quals, ExplainState *es, List* rtable)
{
	/* Set up deparsing context */
	List *ancestors = list_make1(planstate);
	List* context = planstate
		? set_deparse_context_planstate(es->deparse_cxt, (Node *) planstate, ancestors)
		: deparse_context_for_plan_rtable(rtable, select_rtable_names_for_explain(rtable, NULL));
	ListCell* lc;
	StringInfoData buf;
	char const* sep = "";
  	initStringInfo(&buf);

	foreach (lc, quals)
	{
		Node* node = (Node*)lfirst(lc);
		if (IsA(node, RestrictInfo))
			node = (Node*)((RestrictInfo*)node)->clause;
		appendStringInfoString(&buf, sep);
		appendStringInfoString(&buf, deparse_expression(node, context, true, false));
		sep = " and ";
	}
	return buf.data;
}

static char*
PrintExpr(List* quals, List* rtable)
{
	return PrintPlanNode(NULL, quals, NULL, rtable);
}
#endif

static int floor_log(double x)
{
	return (int)log10(x);
}


static void
StoreAQOInfoForNode(PlanState *planstate, ExplainState *es)
{
	Plan	   *plan = planstate->plan;

	/* Do we have actual rows information for this node? */
	if (plan->path_clauses != NULL && planstate->instrument && plan->plan_rows != 0)
	{
		double estimation_error = planstate->instrument->ntuples / plan->plan_rows;
		if (estimation_error >= auto_explain_aqo_threshold) /* this function is called only when auto_explain_aqo_threshold is non-zero */
		{
			AQOHashKey key;
			AQOHashEntry* entry;
			bool found;
			int bin;
			key.predicate = QualsListToString(plan->path_clauses, es->rtable);
			InitAQOHash(); /* Init hash if needed */

			/* LRU */
			if (auto_explain_aqo_limit != 0 && hash_get_num_entries(AQOHash) >= auto_explain_aqo_limit)
			{
				entry = AQOHashLRU.prev;
				LRUListUnlink(entry);
				hash_search(AQOHash, entry, HASH_REMOVE, NULL);
			}
			entry = hash_search(AQOHash, &key, HASH_ENTER, &found);
			bin = Min(floor_log(plan->plan_rows), AQO_MAX_BINS-1);
			Assert(bin >= 0);
			if (found)
			{
				LRUListUnlink(entry);
				ereport(AQO_LOG, (errmsg("AQO: update correction coeffictient for %f from %f to %f for %s %s",
										 plan->plan_rows, entry->correction[bin], estimation_error,
										 AQO_PRETTY_KEY(PrintPlanNode(planstate, plan->path_clauses, es, es->rtable)), key.predicate),
								  errhidestmt(true), errhidecontext(true)));
			}
			else
			{
				ereport(AQO_LOG, (errmsg("AQO: set correction coefficient for %f to %f for %s %s",
										 plan->plan_rows,  estimation_error,
										 AQO_PRETTY_KEY(PrintPlanNode(planstate, plan->path_clauses, es, es->rtable)), key.predicate),
								  errhidestmt(true), errhidecontext(true)));
				memset(entry->correction, 0, sizeof entry->correction);
			}
			entry->correction[bin] = estimation_error;
			LRUListInsert(&AQOHashLRU, entry);
			AQOHashUpdated = true;
		}
	}

	/* lefttree */
	if (outerPlanState(planstate))
		StoreAQOInfoForNode(outerPlanState(planstate), es);

	/* righttree */
	if (innerPlanState(planstate))
		StoreAQOInfoForNode(innerPlanState(planstate), es);

    /* initPlan-s */
	if (planstate->initPlan)
		StoreAQOInfoForSubPlans(planstate->initPlan, es);

	/* special child plans */
	switch (nodeTag(plan))
	{
		case T_ModifyTable:
			StoreAQOInfoForMemberNodes(((ModifyTableState *) planstate)->mt_plans,
									   ((ModifyTableState *) planstate)->mt_nplans,
									   es);
			break;
		case T_Append:
			StoreAQOInfoForMemberNodes(((AppendState *) planstate)->appendplans,
									   ((AppendState *) planstate)->as_nplans,
									   es);
			break;
		case T_MergeAppend:
			StoreAQOInfoForMemberNodes(((MergeAppendState *) planstate)->mergeplans,
									   ((MergeAppendState *) planstate)->ms_nplans,
									   es);
			break;
		case T_BitmapAnd:
			StoreAQOInfoForMemberNodes(((BitmapAndState *) planstate)->bitmapplans,
									   ((BitmapAndState *) planstate)->nplans,
									   es);
			break;
		case T_BitmapOr:
			StoreAQOInfoForMemberNodes(((BitmapOrState *) planstate)->bitmapplans,
									   ((BitmapOrState *) planstate)->nplans,
									   es);
			break;
		case T_SubqueryScan:
			StoreAQOInfoForNode(((SubqueryScanState *) planstate)->subplan, es);
			break;
		default:
			break;
	}
}

static float
AQOGetCorrection(AQOHashEntry* entry, double estimation)
{
	int bin = Min(floor_log(estimation), AQO_MAX_BINS-1);
	float correction = entry->correction[bin];
	if (correction == 0.0 && bin > 0)
		correction = entry->correction[bin-1];
	if (correction == 0.0 && bin < AQO_MAX_BINS-1)
		correction = entry->correction[bin+1];
	return correction == 0.0 ? 1.0 : correction;
}

/*
 * AQO relation size estimation hooks.
 */
static void
AQO_set_joinrel_size_estimates(PlannerInfo *root, RelOptInfo *rel,
							   RelOptInfo *outer_rel,
							   RelOptInfo *inner_rel,
							   SpecialJoinInfo *sjinfo,
							   List *restrict_clauses)
{
	Assert(rel->joininfo == NULL);
	Assert(rel->baserestrictinfo == NULL);

	if (prev_set_joinrel_size_estimates_hook)
		prev_set_joinrel_size_estimates_hook(root, rel,
											outer_rel,
											inner_rel,
											sjinfo,
											restrict_clauses);
	else
		set_joinrel_size_estimates_standard(root, rel,
											outer_rel,
											inner_rel,
											sjinfo,
											restrict_clauses);

	if (HasAQOData() && restrict_clauses && rel->rows >= 1.0)
	{
		AQOHashKey key;
		AQOHashEntry* entry;
		key.predicate = QualsListToString(restrict_clauses, root->parse->rtable);
		entry = (AQOHashEntry*)hash_search(AQOHash, &key, HASH_FIND, NULL);
		if (entry != NULL)
		{
			float correction = AQOGetCorrection(entry, rel->rows);
			double rows = Min(outer_rel->rows * inner_rel->rows, rel->rows * correction);
			Assert(rows >= 1.0);
			ereport(AQO_LOG, (errmsg("AQO: Adjust joinrel estimation %f to %f for %s %s",
									 rel->rows, rows,
									 AQO_PRETTY_KEY(PrintExpr(restrict_clauses, root->parse->rtable)), key.predicate),
							  errhidestmt(true), errhidecontext(true)));
			rel->rows = rows;
		}
		else
			ereport(AQO_LOG, (errmsg("AQO: No runtime info for %s %s",
									 AQO_PRETTY_KEY(PrintExpr(restrict_clauses, root->parse->rtable)), key.predicate),
							  errhidestmt(true), errhidecontext(true)));
	}
}

static void
AQO_set_baserel_rows_estimate(PlannerInfo *root, RelOptInfo *rel)
{
	Assert(rel->joininfo == NULL);
	if (prev_set_baserel_rows_estimate_hook)
		prev_set_baserel_rows_estimate_hook(root, rel);
	else
		set_baserel_rows_estimate_standard(root, rel);

	if (HasAQOData() && rel->baserestrictinfo && rel->rows >= 1.0)
	{
		AQOHashKey key;
		AQOHashEntry* entry;
		key.predicate = QualsListToString(rel->baserestrictinfo, root->parse->rtable);
		entry = (AQOHashEntry*)hash_search(AQOHash, &key, HASH_FIND, NULL);
		if (entry != NULL)
		{
			float correction = AQOGetCorrection(entry, rel->rows);
			double rows = Min(rel->tuples, rel->rows * correction);
			Assert(rows >= 1.0);
			ereport(AQO_LOG, (errmsg("AQO: Adjust baserel estimation %f to %f for %s %s",
									 rel->rows, rows,
									 AQO_PRETTY_KEY(PrintExpr(rel->baserestrictinfo, root->parse->rtable)), key.predicate),
							  errhidestmt(true), errhidecontext(true)));
			rel->rows = rows;
		}
		else
			ereport(AQO_LOG, (errmsg("AQO: No runtime info for %s %s",
									 AQO_PRETTY_KEY(PrintExpr(rel->baserestrictinfo, root->parse->rtable)), key.predicate),
							  errhidestmt(true), errhidecontext(true)));
	}
}

static double
AQO_get_parameterized_joinrel_size(PlannerInfo *root, RelOptInfo *rel,
											 Path *outer_path,
											 Path *inner_path,
											 SpecialJoinInfo *sjinfo,
											 List *restrict_clauses)
{
	double rows = prev_get_parameterized_joinrel_size_hook
		? prev_get_parameterized_joinrel_size_hook(root, rel,
												   outer_path,
												   inner_path,
												   sjinfo,
												   restrict_clauses)
		: get_parameterized_joinrel_size_standard(root, rel,
												  outer_path,
												  inner_path,
												  sjinfo,
												  restrict_clauses);
	if (HasAQOData() && restrict_clauses && rows >= 1.0)
	{
		AQOHashKey key;
		AQOHashEntry* entry;
		key.predicate = QualsListToString(restrict_clauses, root->parse->rtable);
		entry = (AQOHashEntry*)hash_search(AQOHash, &key, HASH_FIND, NULL);
		if (entry != NULL)
		{
			float correction = AQOGetCorrection(entry, rows);
			double fix_rows = Min(outer_path->rows * inner_path->rows, rows * correction);
			Assert(fix_rows >= 1.0);
			ereport(AQO_LOG, (errmsg("AQO: Adjust parameterized joinrel estimation %f to %f for %s %s",
									 rows, fix_rows,
									 AQO_PRETTY_KEY(PrintExpr(restrict_clauses, root->parse->rtable)), key.predicate),
							  errhidestmt(true), errhidecontext(true)));
			rows = fix_rows;
		}
		else
			ereport(AQO_LOG, (errmsg("AQO: No runtime info for %s %s",
									 AQO_PRETTY_KEY(PrintExpr(restrict_clauses, root->parse->rtable)), key.predicate),
							  errhidestmt(true), errhidecontext(true)));
	}
	return rows;
}

static double AQO_get_parameterized_baserel_size(PlannerInfo *root, RelOptInfo *rel,
												 List *param_clauses)
{
	double rows = prev_get_parameterized_baserel_size_hook
		? prev_get_parameterized_baserel_size_hook(root, rel, param_clauses)
		: get_parameterized_baserel_size_standard(root, rel, param_clauses);
	if (HasAQOData() && rows >= 1.0)
	{
		AQOHashKey key;
		AQOHashEntry* entry;

		if (rel->baserestrictinfo)
			param_clauses = list_concat(list_copy(rel->baserestrictinfo), param_clauses);

		key.predicate = QualsListToString(param_clauses, root->parse->rtable);
		entry = (AQOHashEntry*)hash_search(AQOHash, &key, HASH_FIND, NULL);
		if (entry != NULL)
		{
			float correction = AQOGetCorrection(entry, rows);
			double fix_rows = Min(rel->tuples, rows * correction);
			Assert(fix_rows >= 1.0);
			ereport(AQO_LOG, (errmsg("AQO: Adjust parameterized baserel estimation %f to %f for %s %s",
									 rows, fix_rows,
									 AQO_PRETTY_KEY(PrintExpr(param_clauses, root->parse->rtable)), key.predicate),
							  errhidestmt(true), errhidecontext(true)));
			rows = fix_rows;
		}
		else
			ereport(AQO_LOG, (errmsg("AQO: No runtime info for %s %s",
									 AQO_PRETTY_KEY(PrintExpr(param_clauses, root->parse->rtable)), key.predicate),
							  errhidestmt(true), errhidecontext(true)));
	}
	return rows;
}


void
AQO_copy_generic_path_info(Plan *dest, Path *src)
{
	bool		is_join_path =
		(src->type == T_NestPath || src->type == T_MergePath ||	src->type == T_HashPath);

	dest->path_clauses = is_join_path
		? ((JoinPath *) src)->joinrestrictinfo
		: src->param_info
		    ? list_concat(list_copy(src->parent->baserestrictinfo), src->param_info->ppi_clauses)
		    : src->parent->baserestrictinfo;

	if (prev_copy_generic_path_info_hook)
		prev_copy_generic_path_info_hook(dest, src);
}


/*
 * ExecutorEnd hook: log results if needed
 */
static void
explain_ExecutorEnd(QueryDesc *queryDesc)
{
	if (queryDesc->totaltime && auto_explain_enabled())
	{
		double		msec;

		/*
		 * Make sure stats accumulation is done.  (Note: it's okay if several
		 * levels of hook all do this.)
		 */
		InstrEndLoop(queryDesc->totaltime);

		/* Log plan if duration is exceeded. */
		msec = queryDesc->totaltime->total * 1000.0;
		if (msec >= auto_explain_log_min_duration)
		{
			ExplainState *es = NewExplainState();

			es->analyze = (queryDesc->instrument_options && auto_explain_log_analyze);
			es->verbose = auto_explain_log_verbose;
			es->buffers = (es->analyze && auto_explain_log_buffers);
			es->timing = (es->analyze && auto_explain_log_timing);
			es->summary = es->analyze;
			es->format = auto_explain_log_format;
			es->settings = auto_explain_log_settings;

			ExplainBeginOutput(es);
			ExplainQueryText(es, queryDesc);
			ExplainPrintPlan(es, queryDesc);
			if (es->analyze && auto_explain_log_triggers)
				ExplainPrintTriggers(es, queryDesc);
			if (es->costs)
				ExplainPrintJITSummary(es, queryDesc);
			ExplainEndOutput(es);

			/* Add multicolumn statistic if requested */
			if (auto_explain_add_statistics_threshold && !IsParallelWorker())
				AddMultiColumnStatisticsForNode(queryDesc->planstate, es);

			/* Store AQO information is enabled */
			if (auto_explain_aqo_threshold > 0)
				StoreAQOInfoForNode(queryDesc->planstate, es);

			/* Remove last line break */
			if (es->str->len > 0 && es->str->data[es->str->len - 1] == '\n')
				es->str->data[--es->str->len] = '\0';

			/* Fix JSON to output an object */
			if (auto_explain_log_format == EXPLAIN_FORMAT_JSON)
			{
				es->str->data[0] = '{';
				es->str->data[es->str->len - 1] = '}';
			}

			/*
			 * Note: we rely on the existing logging of context or
			 * debug_query_string to identify just which statement is being
			 * reported.  This isn't ideal but trying to do it here would
			 * often result in duplication.
			 */
			ereport(auto_explain_log_level,
					(errmsg("duration: %.3f ms  plan:\n%s",
							msec, es->str->data),
					 errhidestmt(true)));

			pfree(es->str->data);
		}
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}
