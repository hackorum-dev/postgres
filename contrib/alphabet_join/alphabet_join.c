/*-------------------------------------------------------------------------
 *
 * alphabet_join.c
 *	  force tables to be joined in alphabetical order by alias name.
 *    this is just a demonstration, so we don't worry about collation here.
 *
 * Copyright (c) 2016-2024, PostgreSQL Global Development Group
 *
 *	  contrib/alphabet_join/alphabet_join.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "optimizer/paths.h"
#include "parser/parsetree.h"

static void aj_join_path_setup_hook(PlannerInfo *root,
									RelOptInfo *joinrel,
									RelOptInfo *outerrel,
									RelOptInfo *innerrel,
									JoinType jointype,
									JoinPathExtraData *extra);

static join_path_setup_hook_type prev_join_path_setup_hook = NULL;

PG_MODULE_MAGIC;

void
_PG_init(void)
{
	prev_join_path_setup_hook = join_path_setup_hook;
	join_path_setup_hook = aj_join_path_setup_hook;
}

static void
aj_join_path_setup_hook(PlannerInfo *root, RelOptInfo *joinrel,
						RelOptInfo *outerrel, RelOptInfo *innerrel,
						JoinType jointype, JoinPathExtraData *extra)
{
	int			relid;
	char	   *outerrel_last = NULL;

	/* Find the alphabetically last outerrel. */
	relid = -1;
	while ((relid = bms_next_member(outerrel->relids, relid)) >= 0)
	{
		RangeTblEntry *rte = planner_rt_fetch(relid, root);

		Assert(rte->eref != NULL && rte->eref->aliasname != NULL);

		if (outerrel_last == NULL ||
			strcmp(outerrel_last, rte->eref->aliasname) < 0)
			outerrel_last = rte->eref->aliasname;
	}

	/*
	 * If any innerrel is alphabetically before the last outerrel, then this
	 * join order is not alphabetical and should be rejected.
	 */
	relid = -1;
	while ((relid = bms_next_member(innerrel->relids, relid)) >= 0)
	{
		RangeTblEntry *rte = planner_rt_fetch(relid, root);

		Assert(rte->eref != NULL && rte->eref->aliasname != NULL);

		if (strcmp(rte->eref->aliasname, outerrel_last) < 0)
		{
			extra->jsa_mask = 0;
			return;
		}
	}
}
