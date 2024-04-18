/*-------------------------------------------------------------------------
 *
 * uniquekey.c
 *	  Utilities for maintaining uniquekey.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/uniquekey.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/paths.h"


/* Functions to populate UniqueKey */
static bool add_uniquekey_for_uniqueindex(PlannerInfo *root,
										  IndexOptInfo *unique_index,
										  List *truncatable_exprs,
										  List *expr_opfamilies);
static bool unique_ecs_useful_for_distinct(PlannerInfo *root, Bitmapset *ec_indexes);

/* Helper functions to create UniqueKey. */
static UniqueKey * make_uniquekey(Bitmapset *eclass_indexes,
								  bool useful_for_distinct);
static void mark_rel_singlerow(RelOptInfo *rel, int relid);

static UniqueKey * rel_singlerow_uniquekey(RelOptInfo *rel);
static bool uniquekey_contains_multinulls(PlannerInfo *root, RelOptInfo *rel, UniqueKey * ukey);

/* Debug only */
static void print_uniquekey(PlannerInfo *root, RelOptInfo *rel);

static bool uniquekey_contains_in(PlannerInfo *root, UniqueKey * ukey, Bitmapset *ecs, Relids relids);
static bool is_uniquekey_useful_afterjoin(PlannerInfo *root, UniqueKey * ukey, RelOptInfo *joinrel);

/*
 * populate_baserel_uniquekeys
 *
 *		UniqueKey on baserel comes from unique indexes. Any expression
 * which equals with Const can be stripped and the left expressions are
 * still unique.
 */
void
populate_baserel_uniquekeys(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *lc;
	List	   *truncatable_exprs = NIL,
			   *expr_opfamilies = NIL;

	/*
	 * Currently we only use UniqueKey for mark-distinct-as-noop case, so if
	 * there is no-distinct-clause at all, we can ignore the maintenance at
	 * the first place. however for code coverage at the development stage, we
	 * bypass this fastpath on purpose.
	 *
	 * XXX: even we want this fastpath, we still need to distinguish even the
	 * current subquery has no DISTINCT, but the upper query may have.
	 */

	/*
	 * if (root->distinct_pathkeys == NIL) return;
	 */
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		if (rinfo->mergeopfamilies == NIL)
			continue;

		if (!IsA(rinfo->clause, OpExpr))
			continue;

		if (bms_is_empty(rinfo->left_relids))
			truncatable_exprs = lappend(truncatable_exprs, get_rightop(rinfo->clause));
		else if (bms_is_empty(rinfo->right_relids))
			truncatable_exprs = lappend(truncatable_exprs, get_leftop(rinfo->clause));
		else
			continue;

		expr_opfamilies = lappend(expr_opfamilies, rinfo->mergeopfamilies);
	}

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);

		if (!index->unique || !index->immediate ||
			(index->indpred != NIL && !index->predOK))
			continue;

		if (add_uniquekey_for_uniqueindex(root, index,
										  truncatable_exprs,
										  expr_opfamilies))
			/* Find a singlerow case, no need to go through other indexes. */
			return;
	}

	print_uniquekey(root, rel);
}


/*
 * add_uniquekey_for_uniqueindex
 *
 *		 populate a UniqueKey if it is interesting, return true iff the
 * UniqueKey is an SingleRow. Only the interesting UniqueKeys are kept.
 */
static bool
add_uniquekey_for_uniqueindex(PlannerInfo *root, IndexOptInfo *unique_index,
							  List *truncatable_exprs, List *expr_opfamilies)
{
	List	   *unique_exprs = NIL;
	Bitmapset  *unique_ecs = NULL;
	ListCell   *indexpr_item;
	RelOptInfo *rel = unique_index->rel;
	bool		used_for_distinct;
	int			c;

	indexpr_item = list_head(unique_index->indexprs);

	for (c = 0; c < unique_index->nkeycolumns; c++)
	{
		int			attr = unique_index->indexkeys[c];
		Expr	   *expr;		/* The candidate for UniqueKey expression. */
		bool		matched_const = false;
		ListCell   *lc1,
				   *lc2;

		if (attr > 0)
		{
			expr = list_nth_node(TargetEntry, unique_index->indextlist, c)->expr;
		}
		else if (attr == 0)
		{
			/* Expression index */
			expr = lfirst(indexpr_item);
			indexpr_item = lnext(unique_index->indexprs, indexpr_item);
		}
		else					/* attr < 0 */
		{
			/* Index on OID is possible, not handle it for now. */
			return false;
		}

		/* Ignore the expr which are equals to const. */
		forboth(lc1, truncatable_exprs, lc2, expr_opfamilies)
		{
			if (list_member_oid((List *) lfirst(lc2), unique_index->opfamily[c]) &&
				match_index_to_operand((Node *) lfirst(lc1), c, unique_index))
			{
				matched_const = true;
				break;
			}
		}

		if (matched_const)
			continue;

		unique_exprs = lappend(unique_exprs, expr);
	}

	if (unique_exprs == NIL)
	{
		/* single row is always interesting. */
		mark_rel_singlerow(rel, rel->relid);
		return true;
	}

	/*
	 * if no EquivalenceClass is found for any exprs in unique exprs, we are
	 * sure the whole exprs are not in the DISTINCT clause or mergeable join
	 * clauses. so it is not interesting.
	 */
	unique_ecs = build_ec_positions_for_exprs(root, unique_exprs, rel);
	if (unique_ecs == NULL)
		return false;

	used_for_distinct = unique_ecs_useful_for_distinct(root, unique_ecs);


	rel->uniquekeys = lappend(rel->uniquekeys,
							  make_uniquekey(unique_ecs,
											 used_for_distinct));
	return false;
}

/*
 *	make_uniquekey
 *		Based on UnqiueKey rules, it is impossible for a UnqiueKey
 * which have eclass_indexes and relid both set. This function just
 * handle eclass_indexes case.
 */
static UniqueKey *
make_uniquekey(Bitmapset *eclass_indexes, bool useful_for_distinct)
{
	UniqueKey  *ukey = makeNode(UniqueKey);

	ukey->eclass_indexes = eclass_indexes;
	ukey->relid = 0;
	ukey->use_for_distinct = useful_for_distinct;
	return ukey;
}

/*
 * mark_rel_singlerow
 *	mark a relation as singlerow.
 */
static void
mark_rel_singlerow(RelOptInfo *rel, int relid)
{
	UniqueKey  *ukey = makeNode(UniqueKey);

	ukey->relid = relid;
	rel->uniquekeys = list_make1(ukey);
}

static inline bool
uniquekey_is_singlerow(UniqueKey * ukey)
{
	return ukey->relid != 0;
}

/*
 *
 *	Return the UniqueKey if rel is a singlerow Relation. othwise
 * return NULL.
 */
static UniqueKey *
rel_singlerow_uniquekey(RelOptInfo *rel)
{
	if (rel->uniquekeys != NIL)
	{
		UniqueKey  *ukey = linitial_node(UniqueKey, rel->uniquekeys);

		if (ukey->relid)
			return ukey;
	}
	return NULL;
}

/*
 * print_uniquekey
 *	Used for easier reivew, should be removed before commit.
 */
static void
print_uniquekey(PlannerInfo *root, RelOptInfo *rel)
{
	if (!enable_geqo)
	{
		ListCell   *lc;

		elog(INFO, "Rel = %s", bmsToString(rel->relids));
		foreach(lc, rel->uniquekeys)
		{
			UniqueKey  *ukey = lfirst_node(UniqueKey, lc);

			elog(INFO, "UNIQUEKEY{indexes=%s, singlerow_rels=%d, use_for_distinct=%d}",
				 bmsToString(ukey->eclass_indexes),
				 ukey->relid,
				 ukey->use_for_distinct);
		}
	}
}

/*
 *	is it possible that the var contains multi NULL values in the given
 * RelOptInfo rel?
 */
static bool
var_is_nullable(PlannerInfo *root, Var *var, RelOptInfo *rel)
{
	RelOptInfo *base_rel;

	/* check if the outer join can add the NULL values.  */
	if (bms_overlap(var->varnullingrels, rel->relids))
		return true;

	/* check if the user data has the NULL values. */
	base_rel = root->simple_rel_array[var->varno];
	return !bms_is_member(var->varattno - FirstLowInvalidHeapAttributeNumber, base_rel->notnullattrs);
}


/*
 * uniquekey_contains_multinulls
 *
 *	Check if the uniquekey contains nulls values.
 */
static bool
uniquekey_contains_multinulls(PlannerInfo *root, RelOptInfo *rel, UniqueKey * ukey)
{
	int			i = -1;

	while ((i = bms_next_member(ukey->eclass_indexes, i)) >= 0)
	{
		EquivalenceClass *ec = list_nth_node(EquivalenceClass, root->eq_classes, i);
		ListCell   *lc;

		foreach(lc, ec->ec_members)
		{
			EquivalenceMember *em = lfirst_node(EquivalenceMember, lc);
			Var		   *var;

			var = (Var *) em->em_expr;

			if (!IsA(var, Var))
				continue;

			if (var_is_nullable(root, var, rel))
				return true;
			else

				/*
				 * If any one of member in the EC is not nullable, we all the
				 * members are not nullable since they are equal with each
				 * other.
				 */
				break;
		}
	}

	return false;
}


/*
 * relation_is_distinct_for
 *
 * Check if the rel is distinct for distinct_pathkey.
 */
bool
relation_is_distinct_for(PlannerInfo *root, RelOptInfo *rel, List *distinct_pathkey)
{
	ListCell   *lc;
	UniqueKey  *singlerow_ukey = rel_singlerow_uniquekey(rel);
	Bitmapset  *pathkey_bm = NULL;

	if (singlerow_ukey)
	{
		return !uniquekey_contains_multinulls(root, rel, singlerow_ukey);
	}

	foreach(lc, distinct_pathkey)
	{
		PathKey    *pathkey = lfirst_node(PathKey, lc);
		int			pos = list_member_ptr_pos(root->eq_classes, pathkey->pk_eclass);

		if (pos == -1)
			return false;

		pathkey_bm = bms_add_member(pathkey_bm, pos);
	}

	foreach(lc, rel->uniquekeys)
	{
		UniqueKey  *ukey = lfirst_node(UniqueKey, lc);

		if (bms_is_subset(ukey->eclass_indexes, pathkey_bm) &&
			!uniquekey_contains_multinulls(root, rel, ukey))
			return true;
	}

	return false;
}

/*
 * unique_ecs_useful_for_distinct
 *
 *	Return true if all the EquivalenceClass for ecs exists in root->distinct_pathkey.
 */
static bool
unique_ecs_useful_for_distinct(PlannerInfo *root, Bitmapset *ec_indexes)
{
	int			i = -1;

	while ((i = bms_next_member(ec_indexes, i)) >= 0)
	{
		EquivalenceClass *ec = list_nth(root->eq_classes, i);
		ListCell   *p;
		bool		found = false;

		foreach(p, root->distinct_pathkeys)
		{
			PathKey    *pathkey = lfirst_node(PathKey, p);

			if (ec == pathkey->pk_eclass)
			{
				found = true;
				break;
			}
		}
		if (!found)
			return false;
	}
	return true;
}

/*
 * populate_joinrel_uniquekey_for_rel
 *
 *    Check if the pattern of rel.any_column = other_rel.unique_key_column
 * exists, if so, the uniquekey in rel is still valid after join and it is
 * added into joinrel and return true. otherwise return false.
 */
static bool
populate_joinrel_uniquekey_for_rel(PlannerInfo *root, RelOptInfo *joinrel,
								   RelOptInfo *rel, RelOptInfo *other_rel,
								   List *restrictlist)
{
	bool		rel_keep_unique = false;
	Bitmapset  *other_ecs = NULL;
	Relids		other_relids = NULL;
	ListCell   *lc;

	if (rel_singlerow_uniquekey(other_rel))
	{
		/*
		 * any uniquekeys stuff join with single-row, its uniqueness is still
		 * kept.
		 */
		goto done;
	}

	/* find out the ECs which the rel.any_columns equals to. */
	foreach(lc, restrictlist)
	{
		RestrictInfo *r = lfirst_node(RestrictInfo, lc);

		if (r->mergeopfamilies == NIL)
			continue;

		/* Build the Bitmapset for easy comparing. */
		if (bms_equal(r->left_relids, rel->relids) && r->right_ec != NULL)
		{
			other_ecs = bms_add_member(other_ecs, list_member_ptr_pos(root->eq_classes, r->right_ec));
			other_relids = bms_add_members(other_relids, r->right_relids);
		}
		else if (bms_equal(r->right_relids, rel->relids) && r->left_ec != NULL)
		{
			other_ecs = bms_add_member(other_ecs, list_member_ptr_pos(root->eq_classes, r->left_ec));
			other_relids = bms_add_members(other_relids, r->left_relids);
		}
	}

	/* Check if these ECs include a uniquekey of other_rel */
	foreach(lc, other_rel->uniquekeys)
	{
		UniqueKey  *ukey = lfirst_node(UniqueKey, lc);

		if (uniquekey_contains_in(root, ukey, other_ecs, other_relids))
		{
			rel_keep_unique = true;
			break;
		}
	}

	if (!rel_keep_unique)
		return false;

done:

	/*
	 * Now copy the uniquekey in rel to joinrel, but first we need to know if
	 * it is useful.
	 */
	foreach(lc, rel->uniquekeys)
	{
		UniqueKey  *ukey = lfirst_node(UniqueKey, lc);

		if (is_uniquekey_useful_afterjoin(root, ukey, joinrel))
		{
			if (uniquekey_is_singlerow(ukey))
			{
				/*
				 * XXX (?): a). NULL values. b). other relids rather than
				 * ukey->relid.
				 */
				mark_rel_singlerow(joinrel, ukey->relid);
				break;
			}
			joinrel->uniquekeys = lappend(joinrel->uniquekeys, ukey);
		}
	}

	return true;
}

/*
 * populate_joinrel_uniquekeys
 */
void
populate_joinrel_uniquekeys(PlannerInfo *root, RelOptInfo *joinrel,
							RelOptInfo *outerrel, RelOptInfo *innerrel,
							List *restrictlist, JoinType jointype)
{
	bool		outeruk_still_valid = false,
				inneruk_still_valid = false;
	ListCell   *lc,
			   *lc2;

	if (jointype == JOIN_SEMI || jointype == JOIN_ANTI)
	{
		foreach(lc, outerrel->uniquekeys)
		{
			/*
			 * the uniquekey on the outer side is not changed after semi/anti
			 * join.
			 */
			joinrel->uniquekeys = lappend(joinrel->uniquekeys, lfirst(lc));
		}
		return;
	}

	if (outerrel->uniquekeys == NIL || innerrel->uniquekeys == NIL)
		return;

	outeruk_still_valid = populate_joinrel_uniquekey_for_rel(root, joinrel, outerrel,
															 innerrel, restrictlist);
	inneruk_still_valid = populate_joinrel_uniquekey_for_rel(root, joinrel, innerrel,
															 outerrel, restrictlist);

	if (outeruk_still_valid || inneruk_still_valid)

		/*
		 * the uniquekey on outers or inners have been added into joinrel so
		 * the combined uniuqekey from both sides is not needed.
		 */
		return;

	/*
	 * The combined UniqueKey is still unique no matter the join method or
	 * join clauses. So let build the combined ones.
	 */
	foreach(lc, outerrel->uniquekeys)
	{
		UniqueKey  *outer_ukey = lfirst(lc);

		if (!is_uniquekey_useful_afterjoin(root, outer_ukey, joinrel))
			/* discard the uniquekey which is not interesting. */
			continue;

		/* singlerow will make the inneruk_still_valid true */
		Assert(!uniquekey_is_singlerow(outer_ukey));

		foreach(lc2, innerrel->uniquekeys)
		{
			UniqueKey  *inner_ukey = lfirst(lc2);

			if (!is_uniquekey_useful_afterjoin(root, inner_ukey, joinrel))
				continue;

			/* singlerow will make the outeruk_still_valid true */
			Assert(!uniquekey_is_singlerow(inner_ukey));

			joinrel->uniquekeys = lappend(joinrel->uniquekeys,
										  make_uniquekey(
														 bms_union(outer_ukey->eclass_indexes, inner_ukey->eclass_indexes),
														 outer_ukey->use_for_distinct || inner_ukey->use_for_distinct));
		}
	}
}

/*
 * uniquekey_contains_in
 *	Return if UniqueKey contains in the list of EquivalenceClass
 * or the UniqueKey's SingleRow contains in relids.
 */
static bool
uniquekey_contains_in(PlannerInfo *root, UniqueKey * ukey, Bitmapset *ecs, Relids relids)
{

	if (uniquekey_is_singlerow(ukey))
	{
		return bms_is_member(ukey->relid, relids);
	}

	return bms_is_subset(ukey->eclass_indexes, ecs);
}


/*
 * uniquekey_useful_for_merging
 *	Check if the uniquekey is useful for mergejoins above the given relation.
 *
 * similar with pathkeys_useful_for_merging.
 */
static bool
uniquekey_useful_for_merging(PlannerInfo *root, UniqueKey * ukey, RelOptInfo *rel)
{

	int			i = -1;

	while ((i = bms_next_member(ukey->eclass_indexes, i)) >= 0)
	{
		EquivalenceClass *ec = list_nth(root->eq_classes, i);
		ListCell   *j;
		bool		matched = false;

		if (rel->has_eclass_joins && eclass_useful_for_merging(root, ec, rel))
		{
			matched = true;
		}
		else
		{
			foreach(j, rel->joininfo)
			{
				RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(j);

				if (restrictinfo->mergeopfamilies == NIL)
					continue;
				update_mergeclause_eclasses(root, restrictinfo);

				if (ec == restrictinfo->left_ec || ec == restrictinfo->right_ec)
				{
					matched = true;
					break;
				}
			}
		}

		if (!matched)
			return false;
	}

	return true;
}

/*
 * is_uniquekey_useful_afterjoin
 *
 *  uniquekey is useful when it contains in distinct_pathkey or in mergable join clauses.
 */
static bool
is_uniquekey_useful_afterjoin(PlannerInfo *root, UniqueKey * ukey, RelOptInfo *joinrel)
{
	if (ukey->use_for_distinct)
		return true;

	/* XXX might needs a better judgement */
	if (uniquekey_is_singlerow(ukey))
		return true;


	return uniquekey_useful_for_merging(root, ukey, joinrel);
}
