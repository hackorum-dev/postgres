/*-------------------------------------------------------------------------
 *
 * workmem.c
 *
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/workmem/workmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "common/int.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

/* Local variables */

/*
 * A Target represents a collection of data structures, belonging to an
 * execution node, that all share the same memory limit.
 *
 * For example, in parallel query, every parallel worker (plus the leader)
 * gets a copy of the execution node, and therefore a copy of all of that
 * node's work_mem limits. In this case, we'll track a single Target, but its
 * count will include (1 + num_workers), because this Target gets "applied"
 * to (1 + num_workers) execution nodes.
 */
typedef struct Target
{
	/* # of data structures to which target applies: */
	int			count;
	/* workmem estimate for each of these data structures: */
	int			workmem;
	/* (original) workmem limit for each of these data structures: */
	int			limit;
	/* workmem estimate, but capped at (original) workmem limit: */
	int			priority;
	/* ratio of (priority / limit); measure's Target's "greediness": */
	double		ratio;
	/* link to target's actual limit, so we can set it: */
	int		   *target_limit;
}			Target;

typedef struct WorkMemStats
{
	/* total # of data structures that get working memory: */
	uint64		count;
	/* total working memory estimated for this query: */
	uint64		workmem;
	/* total working memory (currently) reserved for this query: */
	uint64		limit;
	/* total "capped" working memory estimate: */
	uint64		priority;
	/* list of Targets, used to update actual workmem limits: */
	List	   *targets;
}			WorkMemStats;

/* GUC variables */
static int	workmem_query_work_mem = 100 * 1024;	/* kB */

/* internal functions */
static void workmem_fn(PlannedStmt *plannedstmt);

static int	clamp_priority(int workmem, int limit);
static Target * make_target(int workmem, int *target_limit, int count);
static void add_target(WorkMemStats * workmem_stats, Target * target);

/* Sort comparators: sort by ratio, ascending or descending. */
static int	target_compare_asc(const ListCell *a, const ListCell *b);
static int	target_compare_desc(const ListCell *a, const ListCell *b);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/* Define custom GUC variable. */
	DefineCustomIntVariable("workmem.query_work_mem",
							"Amount of working-memory (in kB) to provide each "
							"query.",
							NULL,
							&workmem_query_work_mem,
							100 * 1024, /* default to 100 MB */
							64,
							INT_MAX,
							PGC_USERSET,
							GUC_UNIT_KB,
							NULL,
							NULL,
							NULL);

	MarkGUCPrefixReserved("workmem");

	/* Install hooks. */
	ExecAssignWorkMem_hook = workmem_fn;
}

static void
workmem_analyze(PlannedStmt *plannedstmt, WorkMemStats * workmem_stats)
{
	int			idx;

	for (idx = 0; idx < list_length(plannedstmt->workMemCategories); ++idx)
	{
		WorkMemCategory category;
		int			count;
		int			estimate;
		ListCell   *limit_cell;
		int			limit;
		Target	   *target;

		category =
			(WorkMemCategory) list_nth_int(plannedstmt->workMemCategories, idx);
		count = list_nth_int(plannedstmt->workMemCounts, idx);
		estimate = list_nth_int(plannedstmt->workMemEstimates, idx);

		limit = category == WORKMEM_HASH ?
			get_hash_memory_limit() / 1024 : work_mem;
		limit_cell = list_nth_cell(plannedstmt->workMemLimits, idx);
		lfirst_int(limit_cell) = limit;

		target = make_target(estimate, &lfirst_int(limit_cell), count);
		add_target(workmem_stats, target);
	}
}

static void
workmem_set(PlannedStmt *plannedstmt, WorkMemStats * workmem_stats)
{
	int			remaining = workmem_query_work_mem;

	if (workmem_stats->limit <= remaining)
	{
		/*
		 * "High memory" case: we have more than enough query_work_mem; now
		 * hand out the excess.
		 */

		/* This is memory that exceeds workmem limits. */
		remaining -= workmem_stats->limit;

		/*
		 * Sort targets from highest ratio to lowest. When we assign memory to
		 * a Target, we'll truncate fractional KB; so by going through the
		 * list from highest to lowest ratio, we ensure that the lowest ratios
		 * get the leftover fractional KBs.
		 */
		list_sort(workmem_stats->targets, target_compare_desc);

		foreach_ptr(Target, target, workmem_stats->targets)
		{
			double		fraction;
			int			extra_workmem;

			/* How much extra work mem should we assign to this target? */
			fraction = (double) target->workmem / workmem_stats->workmem;

			/* NOTE: This is extra workmem *per data structure*. */
			extra_workmem = (int) (fraction * remaining);

			*target->target_limit += extra_workmem;

			/* OK, we've handled this target. */
			workmem_stats->workmem -= (target->workmem * target->count);
			remaining -= (extra_workmem * target->count);
		}
	}
	else if (workmem_stats->priority <= remaining)
	{
		/*
		 * "Medium memory" case: we don't have enough query_work_mem to give
		 * every target its full allotment, but we do have enough to give it
		 * as much as (we estimate) it needs.
		 *
		 * So, just take some memory away from nodes that (we estimate) won't
		 * need it.
		 */

		/* This is memory that exceeds workmem estimates. */
		remaining -= workmem_stats->priority;

		/*
		 * Sort targets from highest ratio to lowest. We'll skip any Target
		 * with ratio > 1.0, because (we estimate) they already need their
		 * full allotment. Also, once a target reaches its workmem limit,
		 * we'll stop giving it more workmem, leaving the surplus memory to be
		 * assigned to targets with smaller ratios.
		 */
		list_sort(workmem_stats->targets, target_compare_desc);

		foreach_ptr(Target, target, workmem_stats->targets)
		{
			double		fraction;
			int			extra_workmem;

			/* How much extra work mem should we assign to this target? */
			fraction = (double) target->workmem / workmem_stats->workmem;

			/*
			 * Don't give the target more than its (original) limit.
			 *
			 * NOTE: This is extra workmem *per data structure*.
			 */
			extra_workmem = Min((int) (fraction * remaining),
								target->limit - target->priority);

			*target->target_limit = target->priority + extra_workmem;

			/* OK, we've handled this target. */
			workmem_stats->workmem -= (target->workmem * target->count);
			remaining -= (extra_workmem * target->count);
		}
	}
	else
	{
		uint64		limit = workmem_stats->limit;

		/*
		 * "Low memory" case: we are severely memory constrained, and need to
		 * take "priority" memory away from targets that (we estimate)
		 * actually need it. We'll do this by (effectively) reducing the
		 * global "work_mem" limit, uniformly, for all targets, until we're
		 * under the query_work_mem limit.
		 */
		elog(WARNING,
			 "not enough working memory for query: increase "
			 "workmem.query_work_mem");

		/*
		 * Sort targets from lowest ratio to highest. For any target whose
		 * ratio is < the target_ratio, we'll just assign it its priority (=
		 * workmem) as limit, and return the excess workmem to our "limit",
		 * for use by subsequent, greedier, targets.
		 */
		list_sort(workmem_stats->targets, target_compare_asc);

		foreach_ptr(Target, target, workmem_stats->targets)
		{
			double		target_ratio;
			int			target_limit;

			/*
			 * If we restrict our targets to this ratio, we'll stay within the
			 * query_work_mem limit.
			 */
			target_ratio = (double) remaining / limit;

			/*
			 * Don't give this target more than its priority request (but we
			 * might give it less).
			 */
			target_limit = Min(target->priority,
							   target_ratio * target->limit);
			*target->target_limit = target_limit;

			/* "Remaining" decreases by memory we actually assigned. */
			remaining -= (target_limit * target->count);

			/*
			 * "Limit" decreases by target's original memory limit.
			 *
			 * If target_limit <= target->priority, so we restricted this
			 * target to less memory than (we estimate) it needs, then the
			 * target_ratio will stay the same, since, letting A = remaining,
			 * B = limit, and R = ratio, we'll have:
			 *
			 * R=A/B <=> A=R*B <=> A-R*X = R*B - R*X <=> A-R*X = R * (B-X) <=>
			 * R = (A-R*X) / (B-X)
			 *
			 * -- which is what we wanted to prove.
			 *
			 * And if target_limit > target->priority, so we didn't need to
			 * restrict this target beyond its priority estimate, then the
			 * target_ratio will increase. This means more memory for the
			 * remaining, greedier, targets.
			 */
			limit -= (target->limit * target->count);

			target_ratio = (double) remaining / limit;
		}
	}
}

/*
 * workmem_fn: updates the query plan's work_mem based on query_work_mem
 */
static void
workmem_fn(PlannedStmt *plannedstmt)
{
	WorkMemStats workmem_stats;
	MemoryContext context,
				oldcontext;

	/*
	 * We already assigned working-memory limits on the leader, and those
	 * limits were sent to the workers inside the serialized Plan.
	 *
	 * We could re-assign working-memory limits on the parallel worker, to
	 * only those Plan nodes that got sent to the worker, but for now we don't
	 * bother.
	 */
	if (IsParallelWorker())
		return;

	if (workmem_query_work_mem == -1)
		return;					/* disabled */

	memset(&workmem_stats, 0, sizeof(workmem_stats));

	/*
	 * Set up our own memory context, so we can drop the metadata we generate,
	 * all at once.
	 */
	context = AllocSetContextCreate(CurrentMemoryContext,
									"workmem_fn context",
									ALLOCSET_DEFAULT_SIZES);

	oldcontext = MemoryContextSwitchTo(context);

	/* Figure out how much total working memory this query wants/needs. */
	workmem_analyze(plannedstmt, &workmem_stats);

	/* Now restrict the query to workmem.query_work_mem. */
	workmem_set(plannedstmt, &workmem_stats);

	MemoryContextSwitchTo(oldcontext);

	/* Drop all our metadata. */
	MemoryContextDelete(context);
}

static int
clamp_priority(int workmem, int limit)
{
	return Min(workmem, limit);
}

static Target *
make_target(int workmem, int *target_limit, int count)
{
	Target	   *result = palloc_object(Target);

	result->count = count;
	result->workmem = workmem;
	result->limit = *target_limit;
	result->priority = clamp_priority(result->workmem, result->limit);
	result->ratio = (double) result->priority / result->limit;
	result->target_limit = target_limit;

	return result;
}

static void
add_target(WorkMemStats * workmem_stats, Target * target)
{
	workmem_stats->count += target->count;
	workmem_stats->workmem += target->count * target->workmem;
	workmem_stats->limit += target->count * target->limit;
	workmem_stats->priority += target->count * target->priority;
	workmem_stats->targets = lappend(workmem_stats->targets, target);
}

/* This "ascending" comparator sorts least-greedy Targets first. */
static int
target_compare_asc(const ListCell *a, const ListCell *b)
{
	double		a_val = ((Target *) a->ptr_value)->ratio;
	double		b_val = ((Target *) b->ptr_value)->ratio;

	/*
	 * Sort in ascending order: smallest ratio first, then (if ratios equal)
	 * smallest workmem.
	 */
	if (a_val == b_val)
	{
		return pg_cmp_s32(((Target *) a->ptr_value)->workmem,
						  ((Target *) b->ptr_value)->workmem);
	}
	else
		return a_val > b_val ? 1 : -1;
}

/* This "descending" comparator sorts most-greedy Targets first. */
static int
target_compare_desc(const ListCell *a, const ListCell *b)
{
	double		a_val = ((Target *) a->ptr_value)->ratio;
	double		b_val = ((Target *) b->ptr_value)->ratio;

	/*
	 * Sort in descending order: largest ratio first, then (if ratios equal)
	 * largest workmem.
	 */
	if (a_val == b_val)
	{
		return pg_cmp_s32(((Target *) b->ptr_value)->workmem,
						  ((Target *) a->ptr_value)->workmem);
	}
	else
		return b_val > a_val ? 1 : -1;
}
