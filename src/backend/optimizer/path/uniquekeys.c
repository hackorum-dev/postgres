/*-------------------------------------------------------------------------
 *
 * uniquekeys.c
 *	  Utilities for matching and building unique keys
 *
 * Portions Copyright (c) 2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/uniquekeys.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"

/*
 * Examine the rel's restriction clauses for usable var = const clauses
 */
static List*
get_mergeable_const_restrictlist(RelOptInfo *rel)
{
	List	*restrictlist = NIL;
	ListCell	*lc;
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(lc);

		/*
		 * Note: can_join won't be set for a restriction clause, but
		 * mergeopfamilies will be if it has a mergejoinable operator and
		 * doesn't contain volatile functions.
		 */
		if (restrictinfo->mergeopfamilies == NIL)
			continue;			/* not mergejoinable */

		/* XXX can't we check if is a Const */

		/*
		 * The clause certainly doesn't refer to anything but the given rel.
		 * If either side is pseudoconstant then we can use it.
		 */
		if (bms_is_empty(restrictinfo->left_relids))
		{
			/* righthand side is inner */
			restrictinfo->outer_is_left = true;
		}
		else if (bms_is_empty(restrictinfo->right_relids))
		{
			/* lefthand side is inner */
			restrictinfo->outer_is_left = false;
		}
		else
			continue;

		/* OK, add to list */
		restrictlist = lappend(restrictlist, restrictinfo);
	}

	return restrictlist;

}


/*
 * Return true if uk = Const in the restrictlist
 */
static bool
match_index_to_restrictinfo(IndexOptInfo *unique_ind, List *restrictlist)
{
	int c = 0;

	if (restrictlist == NIL)
		return false;

	for(c = 0;  c < unique_ind->nkeycolumns; c++)
	{
		ListCell	*lc;
		foreach(lc, restrictlist)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
			Node	   *rexpr;

			/*
			 * The condition's equality operator must be a member of the
			 * index opfamily, else it is not asserting the right kind of
			 * equality behavior for this index.  We check this first
			 * since it's probably cheaper than match_index_to_operand().
			 */
			if (!list_member_oid(rinfo->mergeopfamilies, unique_ind->opfamily[c]))
				continue;

			/*
			 * XXX at some point we may need to check collations here too.
			 * For the moment we assume all collations reduce to the same
			 * notion of equality.
			 */

			/* OK, see if the condition operand matches the index key */
			if (rinfo->outer_is_left)
				rexpr = get_rightop(rinfo->clause);
			else
				rexpr = get_leftop(rinfo->clause);

			if (!match_index_to_operand(rexpr, c, unique_ind))
			{
				return false;
			}
		}
	}
	return true;
}

/*
 * add_uniquekey_from_index
 * 	We only add the Index Vars whose expr exists in rel->reltarget
 */
static void
add_uniquekey_from_index(RelOptInfo *rel, IndexOptInfo *unique_index)
{
	int	c = 0;
	List	*exprs = NIL;

	/* We only add the index which exists in rel->reltarget */
	for(c = 0; c < unique_index->nkeycolumns; c++)
	{
		ListCell	*lc;
		bool	find_in_exprs = false;
		foreach(lc, rel->reltarget->exprs)
		{
			Var *var;
			/* We never knows a FuncExpr is nullable or not,  we only handle Var now */
			if (!IsA(lfirst(lc), Var))
				continue;
			var = lfirst_node(Var, lc);
			if (var->varattno < InvalidAttrNumber)
				/* System column */
				continue;
			/* Must check not null for unqiue index */
			if (!bms_is_member(var->varattno, rel->not_null_cols))
				continue;

			/* To keep the uniquekey short, We only add it if it exists in rel->reltrget->exprs */
			if (match_index_to_operand((Node *)lfirst(lc), c, unique_index))
			{
				find_in_exprs = true;
				exprs = lappend(exprs, lfirst(lc));
				break;
			}
		}
		if (!find_in_exprs)
			return;
	}
	rel->uniquekeys = lappend(rel->uniquekeys, exprs);
}

/*
 * populate_baserel_uniquekeys
 *		Populate 'baserel' uniquekeys list by looking at the rel's unique index
 * add baserestrictinfo     
 */
void
populate_baserel_uniquekeys(PlannerInfo *root, RelOptInfo *baserel)
{
	ListCell *lc;
	List	*restrictlist = get_mergeable_const_restrictlist(baserel);
	bool	return_one_row = false;
	List	*matched_uk_indexes = NIL;

	Assert(baserel->rtekind == RTE_RELATION);

	foreach(lc, baserel->indexlist)
	{
		IndexOptInfo *ind = (IndexOptInfo *) lfirst(lc);
		/*
		 * If the index is not unique, or not immediately enforced, or if it's
		 * a partial index that doesn't match the query, it's useless here.
		 */
		if (!ind->unique || !ind->immediate ||
			(ind->indpred != NIL && !ind->predOK))
			continue;

		if (match_index_to_restrictinfo(ind, restrictlist))
		{
			return_one_row = true;
			break;
		}
		matched_uk_indexes = lappend(matched_uk_indexes, ind);
	}

	if (return_one_row)
	{
		foreach(lc,  baserel->reltarget->exprs)
		{
			/* Every columns in this relation is unqiue since only 1 row returned
			 * No bother to check it is a var or not, also we don't need the check nullable
			 */
			baserel->uniquekeys = lappend(baserel->uniquekeys,
										  list_make1(lfirst(lc)));
		}
	}
	else
	{
		foreach(lc,   matched_uk_indexes)
			add_uniquekey_from_index(baserel, lfirst_node(IndexOptInfo, lc));
	}
}


/*
 * relation_has_uniquekeys_for
 *		Returns true if we have proofs that 'rel' cannot return multiple rows with
 *		the same values in each of 'exprs'.  Otherwise returns false.
 */
bool
relation_has_uniquekeys_for(PlannerInfo *root, RelOptInfo *rel, List *exprs)
{
	ListCell *lc;

	foreach(lc, rel->uniquekeys)
	{
		List *unique_exprs = lfirst_node(List, lc);
		if (unique_exprs == NIL)
			continue;
		if (list_all_members_in(unique_exprs, exprs))
			return true;
	}
	return false;
}

/*
 * clause_sides_match_join
 *	  Determine whether a join clause is of the right form to use in this join.
 *
 * We already know that the clause is a binary opclause referencing only the
 * rels in the current join.  The point here is to check whether it has the
 * form "outerrel_expr op innerrel_expr" or "innerrel_expr op outerrel_expr",
 * rather than mixing outer and inner vars on either side.  If it matches,
 * we set the transient flag outer_is_left to identify which side is which.
 */
static inline bool
clause_sides_match_join(RestrictInfo *rinfo, Relids outerrelids,
						Relids innerrelids)
{
	if (bms_is_subset(rinfo->left_relids, outerrelids) &&
		bms_is_subset(rinfo->right_relids, innerrelids))
	{
		/* lefthand side is outer */
		rinfo->outer_is_left = true;
		return true;
	}
	else if (bms_is_subset(rinfo->left_relids, innerrelids) &&
			 bms_is_subset(rinfo->right_relids, outerrelids))
	{
		/* righthand side is outer */
		rinfo->outer_is_left = false;
		return true;
	}
	return false;				/* no good for these input relations */
}

/*
 * clauselist_matches_uniquekeys
 *   Detect the pattern that rel1.uk_expr =  rel2.normal_expr in clause_list, 
 * if so, we are sure that the UniqueKey of rel2 still can be unqiue key in joinrel.
 */
static bool
clauselist_matches_uniquekeys(List *clause_list, List *uniquekey,  bool outer_side)
{
	ListCell *lc;

	if (uniquekey == NIL)
		return false;

	foreach(lc, uniquekey)
	{
		Node *expr = (Node *)lfirst(lc);
		ListCell *lc2;
		bool matched_expr = false;

		foreach(lc2, clause_list)
		{
			RestrictInfo *rinfo = (RestrictInfo *)lfirst(lc2);
			Node	   *rexpr;

			/*
			 * The condition's equality operator must be a member of the
			 * index opfamily, else it is not asserting the right kind of
			 * equality behavior for this index.  We check this first
			 * since it's probably cheaper than match_index_to_operand().
			 */
			/* XXXXX do we need this?  uk_opfamily is a Concept when we come to Index
			 * for UniqueKey looks we don't need it
			 **/
			/* if (!list_member_oid(rinfo->mergeopfamilies, key->uk_opfamily)) */
			/* 	continue; */

			/*
			 * XXX at some point we may need to check collations here too.
			 * For the moment we assume all collations reduce to the same
			 * notion of equality.
			 */

			 /* OK, see if the condition operand matches the index key */
			if (rinfo->outer_is_left != outer_side)
				rexpr = get_rightop(rinfo->clause);
			else
				rexpr = get_leftop(rinfo->clause);

			if (IsA(rexpr, RelabelType))
				rexpr = (Node *)((RelabelType *)rexpr)->arg;

			if (equal(rexpr, expr))
			{
				matched_expr = true;
				break;
			}
		}

		if (!matched_expr)
			return false;
	}

	return true;
}

/*
 * Used to record if a uniquekey has been added to joinrel, if so we don't
 * need to add other superset of this uniquekey to the joinrel.
 */
typedef struct UniqueKeyContextData
{
	List	*uniquekey;
	/* Set to true if the unique key has been added to joinrel->uniquekeys */
	bool	added_to_joinrel;
} *UniqueKeyContext;


/*
 * initililze_unqiuecontext_for_joinrel
 * Return a List of UniqueKeyContext for an inputrel, we also filter out 
 * all the unqiuekeys which are not possible to use later
 */
static List *
initililze_unqiuecontext_for_joinrel(RelOptInfo *joinrel,  RelOptInfo *inputrel)
{
	List	*res = NIL;
	ListCell *lc;
	foreach(lc,  inputrel->uniquekeys)
	{
		UniqueKeyContext context;
		/* If it isn't shown in joinrel->reltarget->exprs, it will be not referenced by others */
		if (!list_all_members_in(lfirst_node(List, lc), joinrel->reltarget->exprs))
			continue;
		context = palloc(sizeof(struct UniqueKeyContextData));
		context->uniquekey = lfirst_node(List, lc);
		context->added_to_joinrel = false;
		res = lappend(res, context);
	}
	return res;

}

/*
 * propagate_unique_keys_to_joinrel
 *		Using 'restrictlist' determine if rel2 can duplicate rows in rel1 and
 *		vice-versa.  If the relation at the other side of the join cannot
 *		cause row duplication, then tag the uniquekeys for the relation onto
 *		'joinrel's uniquekey list.
 */
void
propagate_unique_keys_to_joinrel(PlannerInfo *root, RelOptInfo *joinrel,
								 RelOptInfo *rel1, RelOptInfo *rel2,
								 List *restrictlist, JoinType jointype)
{
	ListCell *lc, *lc2;
	List	*clause_list = NIL;
	List	*rel1_uniquekey_context;
	List	*rel2_uniquekey_context;

	/* Care about the left relation only for SEMI/ANTI join */
	if (jointype == JOIN_SEMI || jointype == JOIN_ANTI)
	{
		foreach(lc, rel1->uniquekeys)
		{
			List	*uniquekey = lfirst_node(List, lc);
			if (list_all_members_in(uniquekey, joinrel->reltarget->exprs))
				joinrel->uniquekeys = lappend(joinrel->uniquekeys, uniquekey);
		}
		return;
	}

	rel1_uniquekey_context = initililze_unqiuecontext_for_joinrel(joinrel, rel1);
	rel2_uniquekey_context = initililze_unqiuecontext_for_joinrel(joinrel, rel2);
	
	if (rel1_uniquekey_context == NIL || rel2_uniquekey_context == NIL)
		return;

	foreach(lc, restrictlist)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *)lfirst(lc);


		if (IS_OUTER_JOIN(jointype) &&
			RINFO_IS_PUSHED_DOWN(restrictinfo, joinrel->relids))
			continue;

		/* Ignore if it's not a mergejoinable clause */
		if (!restrictinfo->can_join ||
			restrictinfo->mergeopfamilies == NIL)
			continue;			/* not mergejoinable */

		/*
		 * Check if clause has the form "outer op inner" or "inner op outer",
		 * and if so mark which side is inner.
		 */
		if (!clause_sides_match_join(restrictinfo, rel1->relids, rel2->relids))
			continue;			/* no good for these input relations */

		/* OK, add to list */
		clause_list = lappend(clause_list, restrictinfo);
	}

	foreach(lc, rel1_uniquekey_context)
	{
		List	*uniquekey = ((UniqueKeyContext)lfirst(lc))->uniquekey;
		if (clauselist_matches_uniquekeys(clause_list, uniquekey, true))
		{
			foreach(lc2, rel2_uniquekey_context)
			{
				UniqueKeyContext ctx = (UniqueKeyContext)lfirst(lc);
				joinrel->uniquekeys = lappend(joinrel->uniquekeys, ctx->uniquekey);
				ctx->added_to_joinrel = true;
			}
			break;
		}
	}

	foreach(lc, rel2_uniquekey_context)
	{
		List	*uniquekey = ((UniqueKeyContext)lfirst(lc))->uniquekey;
		if (clauselist_matches_uniquekeys(clause_list, uniquekey, true))
		{
			foreach(lc2, rel1_uniquekey_context)
			{
				UniqueKeyContext ctx = (UniqueKeyContext)lfirst(lc);
				joinrel->uniquekeys = lappend(joinrel->uniquekeys, ctx->uniquekey);
				ctx->added_to_joinrel = true;
			}
			break;
		}
	}

	foreach(lc, rel1_uniquekey_context)
	{
		UniqueKeyContext context1 = (UniqueKeyContext) lfirst(lc);
		if (context1->added_to_joinrel)
			continue;
		foreach(lc2, rel2_uniquekey_context)
		{
			UniqueKeyContext context2 = (UniqueKeyContext) lfirst(lc2);
			List	*uniquekey = NIL;
			if (context2->added_to_joinrel)
				continue;
			uniquekey = list_copy(context1->uniquekey);
			uniquekey = list_concat(uniquekey, context2->uniquekey);
			joinrel->uniquekeys = lappend(joinrel->uniquekeys, uniquekey);
		}
	}
}
