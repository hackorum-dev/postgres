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
#include "optimizer/appendinfo.h"
#include "optimizer/optimizer.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "rewrite/rewriteManip.h"


static List *make_simplified_unique_exprs(PlannerInfo *root,
										  RelOptInfo *rel,
										  List *unique_indexes,
										  bool *matched_whole_index);
static List *filter_useful_unique_exprs(RelOptInfo *rel,
									   List *unique_exprs_list);
static void make_uniquekey_from_exprs(RelOptInfo *rel,
									  List *unique_exprs);
static void make_usual_uniquekey(RelOptInfo *rel, List *exprs,
								 bool multi_nullvals);
static void make_onerow_uniquekey(RelOptInfo *rel);


static List *get_usable_unique_indexes(List *indlist);
static List *extract_const_opposite_exprs(List *restrict_list, List **opfamilies);

/*
 * populate_baserel_uniquekeys
 *		Populate 'baserel' uniquekeys list by looking at the rel's unique index
 * and baserestrictinfo.
 */
void
populate_baserel_uniquekeys(PlannerInfo *root,
							RelOptInfo *rel,
							List *indexlist)
{
	List *unique_indexes = get_usable_unique_indexes(indexlist);
	bool matched_awhole_uniqueindex = false;
	List *unique_exprs_list;

	if (unique_indexes == NIL)
		return;

	unique_exprs_list = make_simplified_unique_exprs(root, rel,
													 unique_indexes,
													 &matched_awhole_uniqueindex);

	if (matched_awhole_uniqueindex)
		make_onerow_uniquekey(rel);
	else
	{
		ListCell	*lc;
		foreach(lc, filter_useful_unique_exprs(rel, unique_exprs_list))
		{
			List	*unique_exprs = lfirst_node(List, lc);
			make_uniquekey_from_exprs(rel, unique_exprs);
		}
	}
}


typedef struct UniqueExprsReduceContext
{
	IndexOptInfo	*indinfo;
	List			*const_opposite_exprs;
	List			*opfamilies;
	List			*res;
} UniqueExprsReduceContext;


static bool
simplified_unique_exprs_walker(Expr *index_expr,
							   int i,
							   UniqueExprsReduceContext *context)
{
	ListCell	*lc1, *lc2;
	bool	found = false;
	forboth(lc1, context->const_opposite_exprs, lc2, context->opfamilies)
	{
		Node	*expr = (Node *)lfirst(lc1);
		List	*expr_families = lfirst(lc2);
		if (list_member_oid(expr_families, context->indinfo->opfamily[i])
			&& match_index_to_operand(expr, i, context->indinfo))
			found = true;
	}
	if (!found)
		context->res = lappend(context->res, index_expr);
	return false;
}


/*
 * make_simplified_unique_exprs
 *
 *	Build a list of exprs/EC from Unique Index, but before that, we would check
 * if any expr is Const already in this query, if so, we would move it out
 * of the exprs. If all the exprs in a unique index match with Consts, we would
 * set *matched_whole_index to true.
 */
static List*
make_simplified_unique_exprs(PlannerInfo *root, RelOptInfo *rel,
							 List *unique_indexes,
							 bool *matched_whole_index)
{
	List	*opfamilies = NIL, *res = NIL;
	List	*clauses = extract_const_opposite_exprs(rel->baserestrictinfo, &opfamilies);
	ListCell	*lc;

	foreach(lc, unique_indexes)
	{
		List *exprs = NIL;
		IndexOptInfo *indinfo = lfirst_node(IndexOptInfo, lc);
		UniqueExprsReduceContext context = {indinfo, clauses, opfamilies, NIL};
		index_keys_walker(indinfo, simplified_unique_exprs_walker, &context);
		if (context.res == NIL)
		{
			*matched_whole_index = true;
			return NIL;
		}

		foreach(lc, context.res)
		{
			Expr *expr = lfirst(lc);
			EquivalenceClass *ec = find_ec_for_expr(root, expr, rel);
			if (ec)
			    exprs = lappend(exprs, ec);
			else
				exprs = lappend(exprs, expr);
		}

		res = lappend(res, exprs);
	}
	return res;
}


static List*
get_usable_unique_indexes(List *indlist)
{
	ListCell *lc;
	List *res = NIL;
	foreach(lc, indlist)
	{
		IndexOptInfo *ind = (IndexOptInfo *) lfirst(lc);
		if (!ind->unique || !ind->immediate ||
			(ind->indpred != NIL && !ind->predOK))
			continue;
		res = lappend(res, ind);
	}
	return res;
}



/*
 * make_usual_uniquekey
 */
static void
make_usual_uniquekey(RelOptInfo *rel, List *exprs, bool multi_nullvals)
{
	UniqueKey * ukey = makeNode(UniqueKey);
	ukey->exprs = exprs;
	ukey->multi_nullvals = multi_nullvals;
	rel->uniquekeys = lappend(rel->uniquekeys, ukey);
}

static void
make_onerow_uniquekey(RelOptInfo *rel)
{
	UniqueKey *ukey = makeNode(UniqueKey);
	ukey->exprs = NIL;
	ukey->multi_nullvals = false;
	rel->uniquekeys = list_make1(ukey);
}

static List *
extract_const_opposite_exprs(List *restrict_list, List **opfamilies)
{
	ListCell	*lc;
	List		*res = NIL;
	foreach(lc, restrict_list)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		Node *leftop, *rightop;

		if (rinfo->mergeopfamilies == NIL || rinfo->pseudoconstant)
			continue;

		leftop = get_leftop(rinfo->clause);
		rightop = get_rightop(rinfo->clause);

		if (leftop == NULL || rightop == NULL)
			continue;

		if (bms_is_empty(rinfo->left_relids) && !contain_volatile_functions(leftop))
		{
			res = lappend(res, rightop);
			*opfamilies = lappend(*opfamilies, rinfo->mergeopfamilies);
		}
		else if (bms_is_empty(rinfo->right_relids) && !contain_volatile_functions(rightop))
		{
			res = lappend(res, leftop);
			*opfamilies = lappend(*opfamilies, rinfo->mergeopfamilies);
		}
	}
	return res;
}



static bool
can_expr_be_referenced(RelOptInfo *rel, Expr *expr, Bitmapset *known_varattrs)
{
	if (IsA(expr, Var))
		return bms_is_member(((Var*)expr)->varattno - FirstLowInvalidHeapAttributeNumber,
							 known_varattrs);

	return list_member(rel->reltarget->exprs, expr);
}

/*
 * filter_useful_unique_exprs
 *
 *	Filter out any exprs which is impossible to be referenced later.
 */
static List *
filter_useful_unique_exprs(RelOptInfo *rel, List *unique_exprs_list)
{
	List	*res = NIL;
	ListCell	*lc;

	Bitmapset *target_attrs = NULL;
	foreach(lc, rel->reltarget->exprs)
	{
		Expr *expr = lfirst(lc);
		if (IsA(expr, Var))
		{
			/* TODO: check RelOptInfo->attr_needed */
			Var *var = (Var *)expr;
			target_attrs = bms_add_member(target_attrs,
										  var->varattno - FirstLowInvalidHeapAttributeNumber);
		}
	}

	foreach(lc, unique_exprs_list)
	{
		List	*unique_exprs = lfirst_node(List, lc);
		ListCell	*l;
		bool	useful = true;
		foreach(l, unique_exprs)
		{
			Expr *node = lfirst(l);
			if (IsA(node, EquivalenceClass))
			{
				EquivalenceClass *ec = (EquivalenceClass *)node;
				ListCell	*lc2;
				bool any_matchs = false;
				foreach(lc2, ec->ec_members)
				{
					Expr *expr = lfirst_node(EquivalenceMember, lc2)->em_expr;
					if (can_expr_be_referenced(rel, expr, target_attrs))
					{
						any_matchs = true;
						break;
					}
				}
				if (!any_matchs)
					useful = false;
			}
			else
				useful = can_expr_be_referenced(rel, node, target_attrs);

			if (!useful)
				break;
		}
		if (useful)
			res = lappend(res, unique_exprs);
	}
	return res;
}


static bool
is_nullable_expr(Expr *expr, RelOptInfo *rel)
{
	if (IsA(expr, Var))
	{
		Var *var = (Var *)expr;
		return !bms_is_member(var->varattno - FirstLowInvalidHeapAttributeNumber,
							  rel->notnullattrs);
	}
	/* Actually we can check more with baserestrictinfo, joininfo. */
	return true;
}


/*
 * make_uniquekey_from_exprs
 *
 *  translate expr to EquivalenceClass if possible before creating
 * a real UniqueKey.
 */
static void
make_uniquekey_from_exprs(RelOptInfo *rel,
						  List *unique_exprs)
{
	ListCell	*lc;
	bool	maynull = false;
	foreach(lc, unique_exprs)
	{
		Expr *expr = lfirst(lc);
		EquivalenceMember *em;

		if (IsA(expr, EquivalenceClass))
		{
			EquivalenceClass *ec = (EquivalenceClass *)expr;
			if (list_length(ec->ec_members) == 1)
			{
				/*
				 * An non-1-member EC indicates it is not null.
				 * Need double check with partitioned case.
				 */
				em = linitial_node(EquivalenceMember, ec->ec_members);
				maynull = is_nullable_expr(em->em_expr, rel);
			}
		}
		else
			maynull = is_nullable_expr(expr, rel);

		if (maynull)
			break;
	}
	make_usual_uniquekey(rel, unique_exprs,  maynull);
}

/*
 * any_ec_member_in_exprs
 */
static bool
any_ec_member_in_exprs(EquivalenceClass *ec, List *exprs)
{
	ListCell	*lc;
	foreach(lc, ec->ec_members)
	{
		if (list_member(exprs, lfirst_node(EquivalenceMember, lc)->em_expr))
			return true;
	}
	return false;
}

/*
 * Check if the exprs in UniqueKey are a subset of exprs
 */
static bool
are_exprs_match_uniquekey(List *exprs, UniqueKey *ukey)
{
	ListCell	*lc;
	foreach(lc, ukey->exprs)
	{
		Expr *expr = lfirst(lc);
		if (IsA(expr, EquivalenceClass))
		{
			if (!any_ec_member_in_exprs((EquivalenceClass *)expr, exprs))
				return false;
		}
		else
			if (!list_member(exprs, expr))
				return false;
	}
	return true;
}


bool
relation_has_uniquekeys_for(List *exprs, RelOptInfo *rel, bool allow_multinulls)
{
	ListCell *lc;
	if (rel->uniquekeys == NIL)
		return false;

	foreach(lc, rel->uniquekeys)
	{
		UniqueKey *ukey = lfirst_node(UniqueKey, lc);
		if (!allow_multinulls && ukey->multi_nullvals)
			continue;
		if (are_exprs_match_uniquekey(exprs, ukey))
			return true;
	}
	return false;
}
