/*-------------------------------------------------------------------------
 *
 * hint_via_alias.c
 *	  force tables to be joined in using a nestedloop, mergejoin, or hash
 *    join if their alias name begins with nl_, mj_, or hj_.
 *
 *    forces tables to be sequential scanned, index scanned, index only
 *    scanned, bitmap scanned, or tid scanned if their alias name begins
 *    with ss_, is_, ios_, bs_, or ts_.
 *
 * Copyright (c) 2016-2024, PostgreSQL Global Development Group
 *
 *	  contrib/hint_via_alias/hint_via_alias.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "parser/parsetree.h"

typedef enum
{
	HVAJ_UNSPECIFIED,
	HVAJ_HASHJOIN,
	HVAJ_MERGEJOIN,
	HVAJ_NESTLOOP
} hvaj_hint;

static void hva_get_relation_info(PlannerInfo *root, Oid relationObjectId,
								  bool inhparent, RelOptInfo *rel);
static void hva_join_path_setup(PlannerInfo *root,
								RelOptInfo *joinrel,
								RelOptInfo *outerrel,
								RelOptInfo *innerrel,
								JoinType jointype,
								JoinPathExtraData *extra);
static hvaj_hint get_join_hint(PlannerInfo *root, Index relid);
static uint32 get_scan_hint(PlannerInfo *root, Index relid);

static get_relation_info_hook_type prev_get_relation_info_hook = NULL;
static join_path_setup_hook_type prev_join_path_setup_hook = NULL;

PG_MODULE_MAGIC;

void
_PG_init(void)
{
	prev_get_relation_info_hook = get_relation_info_hook;
	get_relation_info_hook = hva_get_relation_info;
	prev_join_path_setup_hook = join_path_setup_hook;
	join_path_setup_hook = hva_join_path_setup;
}

static void
hva_get_relation_info(PlannerInfo *root, Oid relationObjectId,
					  bool inhparent, RelOptInfo *rel)
{
	uint32		hint = get_scan_hint(root, rel->relid);

	if (hint != 0)
		rel->ssa_mask &= hint;

	if (prev_get_relation_info_hook != NULL)
		prev_get_relation_info_hook(root, relationObjectId, inhparent, rel);
}

static void
hva_join_path_setup(PlannerInfo *root, RelOptInfo *joinrel,
					RelOptInfo *outerrel, RelOptInfo *innerrel,
					JoinType jointype, JoinPathExtraData *extra)
{
	hvaj_hint	outerhint = HVAJ_UNSPECIFIED;
	hvaj_hint	innerhint = HVAJ_UNSPECIFIED;
	hvaj_hint	hint;

	if (outerrel->reloptkind == RELOPT_BASEREL ||
		outerrel->reloptkind == RELOPT_OTHER_MEMBER_REL)
		outerhint = get_join_hint(root, outerrel->relid);

	if (innerrel->reloptkind == RELOPT_BASEREL ||
		innerrel->reloptkind == RELOPT_OTHER_MEMBER_REL)
		innerhint = get_join_hint(root, innerrel->relid);

	/*
	 * If the hints conflict, that's not necessarily an indication of user
	 * error. For example, if the user joins A to B and supplies different
	 * join method hints for A and B, we will end up using a disabled path.
	 * However, if they are joining A, B, and C and supply different join
	 * method hints for A and B, we could potentially respect both hints by
	 * avoiding a direct A-B join altogether. Even if it does turn out that we
	 * can't respect all the hints, we don't need any special handling for
	 * that here: the planner will just return a disabled path.
	 */
	if (outerhint != HVAJ_UNSPECIFIED && innerhint != HVAJ_UNSPECIFIED &&
		outerhint != innerhint)
	{
		extra->jsa_mask = 0;
		return;
	}

	if (outerhint != HVAJ_UNSPECIFIED)
		hint = outerhint;
	else
		hint = innerhint;

	switch (hint)
	{
		case HVAJ_UNSPECIFIED:
			break;
		case HVAJ_NESTLOOP:
			extra->jsa_mask &= JSA_NESTLOOP_ANY;
			break;
		case HVAJ_HASHJOIN:
			extra->jsa_mask &= JSA_HASHJOIN;
			break;
		case HVAJ_MERGEJOIN:
			extra->jsa_mask &= JSA_MERGEJOIN_ANY;
			break;
	}

	if (prev_join_path_setup_hook != NULL)
		prev_join_path_setup_hook(root, joinrel, outerrel, innerrel, jointype,
								  extra);
}

static hvaj_hint
get_join_hint(PlannerInfo *root, Index relid)
{
	RangeTblEntry *rte = planner_rt_fetch(relid, root);

	Assert(rte->eref != NULL && rte->eref->aliasname != NULL);

	if (strncmp(rte->eref->aliasname, "nl_", 3) == 0)
		return HVAJ_NESTLOOP;
	else if (strncmp(rte->eref->aliasname, "hj_", 3) == 0)
		return HVAJ_HASHJOIN;
	else if (strncmp(rte->eref->aliasname, "mj_", 3) == 0)
		return HVAJ_MERGEJOIN;
	else
		return HVAJ_UNSPECIFIED;
}

static uint32
get_scan_hint(PlannerInfo *root, Index relid)
{
	RangeTblEntry *rte = planner_rt_fetch(relid, root);

	/* happens if CREATE INDEX is used without an index name, at least */
	if (rte->eref == NULL)
		return 0;

	Assert(rte->eref->aliasname != NULL);

	if (strncmp(rte->eref->aliasname, "ts_", 3) == 0)
		return SSA_TIDSCAN;
	else if (strncmp(rte->eref->aliasname, "ss_", 3) == 0)
		return SSA_SEQSCAN;
	else if (strncmp(rte->eref->aliasname, "is_", 3) == 0)
		return SSA_INDEXSCAN;
	else if (strncmp(rte->eref->aliasname, "ios_", 3) == 0)
		return SSA_INDEXONLYSCAN | SSA_CONSIDER_INDEXONLY;
	else if (strncmp(rte->eref->aliasname, "bs_", 3) == 0)
		return SSA_BITMAPSCAN;
	else
		return 0;
}
