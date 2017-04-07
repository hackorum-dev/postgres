/*-------------------------------------------------------------------------
 *
 * clausesel.c
 *	  Routines to compute clause selectivities
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/clausesel.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "statistics/statistics.h"

#define CACHESEL_LOBOUND		0x0001	/* has var > something selectivity */
#define CACHESEL_HIBOUND		0x0002	/* has var < something selectivity */
#define CACHESEL_NULLTEST		0x0004	/* has var IS NULL selectivity */
#define CACHESEL_NOTNULLTEST	0x0008	/* has var IS NOT NULL selectivity */
#define CACHESEL_OTHERSTRICT	0x0010	/* has another OpExpr selectivity */

/*
 * Data structure for caching selectivity for clauselist_selectivity.
 */
typedef struct CachedSelectivityClause
{
	struct CachedSelectivityClause *next;		/* next in linked list */
	Node	   *var;			/* The common variable of the clauses */
	int			selmask;		/* Bitmask of which sel types are stored */
	Selectivity lobound;		/* Selectivity of a var > something clause */
	Selectivity hibound;		/* Selectivity of a var < something clause */
	Selectivity nullsel;		/* Selectivity of a IS NULL test */
	Selectivity notnullsel;		/* Selectivity of a IS NOT NULL test */
	Selectivity otherstrictsel; /* Selectivity of any other strict clauses */
} CachedSelectivityClause;


static CachedSelectivityClause *findCachedSelectivityVar(
						 CachedSelectivityClause **cslist, Node *expr);
static void addCachedSelectivityNullTest(CachedSelectivityClause **cslist,
							 Node *expr, Selectivity s2);
static void addCachedSelectivityNotNullTest(CachedSelectivityClause **cslist,
								Node *expr, Selectivity s2);
static void addCachedSelectivityRangeVar(CachedSelectivityClause **cslist,
				   Node *expr, bool varonleft, bool isLTsel, Selectivity s2);
static void addCachedSelectivityOtherStrictClause(
									  CachedSelectivityClause **cslist,
									  Node *expr, Selectivity s2);
static RelOptInfo *find_relation_from_clauses(PlannerInfo *root,
											  List *clauses);

/****************************************************************************
 *		ROUTINES TO COMPUTE SELECTIVITIES
 ****************************************************************************/

/*
 * clauselist_selectivity -
 *	  Compute the selectivity of an implicitly-ANDed list of boolean
 *	  expression clauses.  The list can be empty, in which case 1.0
 *	  must be returned.  List elements may be either RestrictInfos
 *	  or bare expression clauses --- the former is preferred since
 *	  it allows caching of results.
 *
 * See clause_selectivity() for the meaning of the additional parameters.
 *
 * Our basic approach is to take the product of the selectivities of the
 * subclauses.  However, that's only right if the subclauses have independent
 * probabilities, and in reality they are often NOT independent.  So,
 * we want to be smarter where we can.
 *
 * When 'rel' is not null and rtekind = RTE_RELATION, we'll try to apply
 * selectivity estimates using any extended statistcs on 'rel'.
 *
 * If we identify such extended statistics exist, we try to apply them.
 * Currently we only have (soft) functional dependencies, so apply these in as
 * many cases as possible, and fall back on normal estimates for remaining
 * clauses.
 *
 * We also recognize "range queries", such as "x > 34 AND x < 42".  Clauses
 * are recognized as possible range query components if they are restriction
 * opclauses whose operators have scalarltsel() or scalargtsel() as their
 * restriction selectivity estimator.  We pair up clauses of this form that
 * refer to the same variable.  An unpairable clause of this kind is simply
 * multiplied into the selectivity product in the normal way.  But when we
 * find a pair, we know that the selectivities represent the relative
 * positions of the low and high bounds within the column's range, so instead
 * of figuring the selectivity as hisel * losel, we can figure it as hisel +
 * losel - 1.  (To visualize this, see that hisel is the fraction of the range
 * below the high bound, while losel is the fraction above the low bound; so
 * hisel can be interpreted directly as a 0..1 value but we need to convert
 * losel to 1-losel before interpreting it as a value.  Then the available
 * range is 1-losel to hisel.  However, this calculation double-excludes
 * nulls, so really we need hisel + losel + null_frac - 1.).
 *
 * IS [NOT] NULL conditions are also handled in a special way. We attempt to
 * cache all quals on a given expression and only apply the selectivity of an
 * IS [NOT] NULL condition if no other qual on the same expression would
 * have already accounted for nulls be filtered, for example;
 *
 * WHERE x = 1 AND x IS NOT NULL;
 *
 * the x = 1 will have already included filtering the NULL values during its
 * selectivity estimate, if we went and applied the x IS NOT NULL selectivity
 * again, then we'd end up underestimating the selectivity over both quals.
 *
 * For range pairs, if either selectivity is exactly DEFAULT_INEQ_SEL, we
 * forget this equation and instead use DEFAULT_RANGE_INEQ_SEL.  The same
 * applies if the equation yields an impossible (negative) result.
 *
 * A free side-effect is that we can recognize redundant inequalities such
 * as "x < 4 AND x < 5"; only the tighter constraint will be counted.
 *
 * Of course this is all very dependent on the behavior of
 * scalarltsel/scalargtsel; perhaps some day we can generalize the approach.
 */
Selectivity
clauselist_selectivity(PlannerInfo *root,
					   List *clauses,
					   int varRelid,
					   JoinType jointype,
					   SpecialJoinInfo *sjinfo)
{
	Selectivity s1 = 1.0;
	CachedSelectivityClause *cslist = NULL;
	ListCell   *l;
	Bitmapset  *estimatedclauses = NULL;
	int			listidx;
	RelOptInfo *rel;

	/*
	 * If there's exactly one clause, then extended statistics is futile at
	 * this level (we might be able to apply them later if it's AND/OR
	 * clause). So just go directly to clause_selectivity().
	 */
	if (list_length(clauses) == 1)
		return clause_selectivity(root, (Node *) linitial(clauses),
								  varRelid, jointype, sjinfo);

	/*
	 * Determine if these clauses reference a single relation. If so we might
	 * like to try applying any extended statistics which exist on it.
	 * Called many time during joins, so must return NULL quickly if not.
	 */
	rel = find_relation_from_clauses(root, clauses);

	/*
	 * When a relation of RTE_RELATION is given as 'rel', we'll try to
	 * perform selectivity estimation using extended statistics.
	 */
	if (rel && rel->rtekind == RTE_RELATION && rel->statlist != NIL)
	{
		/*
		 * Perform selectivity estimations on any clauses found applicable by
		 * dependencies_clauselist_selectivity. The 0-based list position of
		 * estimated clauses will be populated in 'estimatedclauses'.
		 */
		s1 *= dependencies_clauselist_selectivity(root, clauses, varRelid,
								   jointype, sjinfo, rel, &estimatedclauses);

		/*
		 * This would be the place to apply any other types of extended
		 * statistics selectivity estimations for remaining clauses.
		 */
	}

	/*
	 * Apply normal selectivity estimates for remaining clauses. We'll be
	 * careful to skip any clauses which were already estimated above.
	 *
	 * Anything that doesn't look like a potential rangequery clause gets
	 * multiplied into s1 and forgotten. Anything that does gets inserted into
	 * an rqlist entry.
	 */
	listidx = -1;
	foreach(l, clauses)
	{
		Node	   *clause = (Node *) lfirst(l);
		RestrictInfo *rinfo;
		Selectivity s2;

		listidx++;

		/*
		 * Skip this clause if it's already been estimated by some other
		 * statistics above.
		 */
		if (bms_is_member(listidx, estimatedclauses))
			continue;

		/* Always compute the selectivity using clause_selectivity */
		s2 = clause_selectivity(root, clause, varRelid, jointype, sjinfo);

		/*
		 * Check for being passed a RestrictInfo.
		 *
		 * If it's a pseudoconstant RestrictInfo, then s2 is either 1.0 or
		 * 0.0; just use that rather than looking for range pairs.
		 */
		if (IsA(clause, RestrictInfo))
		{
			rinfo = (RestrictInfo *) clause;
			if (rinfo->pseudoconstant)
			{
				s1 = s1 * s2;
				continue;
			}
			clause = (Node *) rinfo->clause;
		}
		else
			rinfo = NULL;

		if (IsA(clause, NullTest))
		{
			NullTestType nulltesttype = ((NullTest *) clause)->nulltesttype;

			if (nulltesttype == IS_NULL)
				addCachedSelectivityNullTest(&cslist,
									(Node *) ((NullTest *) clause)->arg, s2);
			else if (nulltesttype == IS_NOT_NULL)
				addCachedSelectivityNotNullTest(&cslist,
									(Node *) ((NullTest *) clause)->arg, s2);

			continue;			/* drop to loop bottom */
		}

		/*
		 * See if it looks like a restriction clause with a pseudoconstant on
		 * one side.  (Anything more complicated than that might not behave in
		 * the simple way we are expecting.)  Most of the tests here can be
		 * done more efficiently with rinfo than without.
		 */
		if (is_opclause(clause) && list_length(((OpExpr *) clause)->args) == 2)
		{
			OpExpr	   *expr = (OpExpr *) clause;
			bool		varonleft = true;
			bool		ok;
			Node	   *leftexpr = (Node *) linitial(expr->args);
			Node	   *rightexpr = (Node *) lsecond(expr->args);

			if (rinfo)
			{
				ok = (bms_membership(rinfo->clause_relids) == BMS_SINGLETON) &&
					(is_pseudo_constant_clause_relids(rightexpr,
													  rinfo->right_relids) ||
					 (varonleft = false,
					  is_pseudo_constant_clause_relids(leftexpr,
													   rinfo->left_relids)));
			}
			else
			{
				ok = (NumRelids(clause) == 1) &&
					(is_pseudo_constant_clause(rightexpr) ||
					 (varonleft = false,
					  is_pseudo_constant_clause(leftexpr)));
			}

			if (ok)
			{
				Node	   *var;

				if (varonleft)
					var = leftexpr;
				else
					var = rightexpr;

				/*
				 * If it's not a "<" or ">" operator, just merge the
				 * selectivity in generically.  But if it's the right oprrest,
				 * add the clause to rqlist for later processing.
				 */
				switch (get_oprrest(expr->opno))
				{
					case F_SCALARLTSEL:
						addCachedSelectivityRangeVar(&cslist, var,
													 varonleft, true, s2);
						break;
					case F_SCALARGTSEL:
						addCachedSelectivityRangeVar(&cslist, var,
													 varonleft, false, s2);
						break;
					default:

						/*
						 * Cache all strict clauses in selectivity other.
						 * Anything non-strict we'll just apply the
						 * selectivity now, since we're currently unable to do
						 * anything particularly smart with it.
						 */
						if (op_strict(expr->opno))
							addCachedSelectivityOtherStrictClause(&cslist, var, s2);
						else
							s1 = s1 * s2;
						break;
				}
				continue;		/* drop to loop bottom */
			}
		}

		/* Not the right form, so treat it generically. */
		s1 = s1 * s2;
	}

	/*
	 * Now scan the cached selectivity list
	 */
	while (cslist != NULL)
	{
		CachedSelectivityClause *csnext;

		if ((cslist->selmask & CACHESEL_NULLTEST))
		{
			/*
			 * if null test is not the only flag then there can be no matching
			 * rows at all.
			 */
			if (cslist->selmask != CACHESEL_NULLTEST)
			{
				s1 = 0;
				break;			/* nothing more needs estimated */
			}
			else
				s1 *= cslist->nullsel;
		}

		/*
		 * An IS NOT NULL test is a no-op if there's any other strict quals,
		 * so if that's the case, then we'll only apply this, otherwise we'll
		 * ignore it.
		 */
		else if (cslist->selmask == CACHESEL_NOTNULLTEST)
			s1 *= cslist->notnullsel;

		else
		{
			/* Check if both lobound and hibound were seen */
			if ((cslist->selmask & (CACHESEL_LOBOUND | CACHESEL_HIBOUND)) ==
				(CACHESEL_LOBOUND | CACHESEL_HIBOUND))
			{
				/* Successfully matched a pair of range clauses */
				Selectivity s2;

				/*
				 * Exact equality to the default value probably means the
				 * selectivity function punted.  This is not airtight but
				 * should be good enough.
				 */
				if (cslist->hibound == DEFAULT_INEQ_SEL ||
					cslist->lobound == DEFAULT_INEQ_SEL)
				{
					s2 = DEFAULT_RANGE_INEQ_SEL;
				}
				else
				{
					Selectivity nullsel;
					Selectivity notnullsel;

					s2 = cslist->hibound + cslist->lobound - 1.0;

					nullsel = nulltestsel(root, IS_NULL, cslist->var, varRelid,
										  jointype, sjinfo);
					notnullsel = 1.0 - nullsel;

					/* Adjust for double-exclusion of NULLs */
					s2 += nullsel;

					/*
					 * A zero or slightly negative s2 should be converted into
					 * a small positive value; we probably are dealing with a
					 * very tight range and got a bogus result due to roundoff
					 * errors. However, if s2 is very negative, then we
					 * probably have default selectivity estimates on one or
					 * both sides of the range that we failed to recognize
					 * above for some reason.
					 */
					if (s2 <= 0.0)
					{
						if (s2 <= (-1.0e-4 * notnullsel - 1.0e-6))
						{
							/* Most likely contradictory clauses found */
							s2 = Min(1.0e-10, notnullsel);
						}
						else
						{
							/* Could be a rounding error */
							s2 = DEFAULT_RANGE_INEQ_SEL * notnullsel;
						}
					}
				}
				/* Merge in the selectivity of the pair of clauses */
				s1 *= s2;
			}
			else
			{
				/* Only found one of a range pair, merge it in generically */
				if ((cslist->selmask & CACHESEL_LOBOUND))
					s1 *= cslist->lobound;
				else if ((cslist->selmask & CACHESEL_HIBOUND))
					s1 *= cslist->hibound;
			}

			/* apply the selectivity for any other seen strict qual */
			if ((cslist->selmask & CACHESEL_OTHERSTRICT))
				s1 *= cslist->otherstrictsel;
		}

		/* release storage and advance */
		csnext = cslist->next;
		pfree(cslist);
		cslist = csnext;
	}

	return s1;
}

/*
 * findCachedSelectivityVar
 *	Find existing seletivity var, or add this var to the list.
 */
static CachedSelectivityClause *
findCachedSelectivityVar(CachedSelectivityClause **cslist, Node *expr)
{
	CachedSelectivityClause *cselem;

	for (cselem = *cslist; cselem; cselem = cselem->next)
	{
		/*
		 * We use full equal() here because the "var" might be a function of
		 * one or more attributes of the same relation...
		 */
		if (equal(expr, cselem->var))
			return cselem;
	}

	/* not found -- add it */
	cselem = (CachedSelectivityClause *) palloc(sizeof(CachedSelectivityClause));
	cselem->var = expr;
	cselem->selmask = 0;

	cselem->next = *cslist;
	*cslist = cselem;
	return cselem;
}


/*
 * addCachedSelectivityNullTest
 *		Cache selectivity for an IS NULL test.
 */
static void
addCachedSelectivityNullTest(CachedSelectivityClause **cslist, Node *expr,
							 Selectivity s2)
{
	CachedSelectivityClause *cselem;

	cselem = findCachedSelectivityVar(cslist, expr);

	/* We can simply overwrite any previously cached selectivity here */
	cselem->nullsel = s2;
	cselem->selmask |= CACHESEL_NULLTEST;
}

/*
 * addCachedSelectivityNotNullTest
 *		Cache selectivity for an IS NOT NULL test.
 */
static void
addCachedSelectivityNotNullTest(CachedSelectivityClause **cslist, Node *expr,
								Selectivity s2)
{
	CachedSelectivityClause *cselem;

	cselem = findCachedSelectivityVar(cslist, expr);

	/* We can simply overwrite any previously cached selectivity here */
	cselem->notnullsel = s2;
	cselem->selmask |= CACHESEL_NOTNULLTEST;
}

/*
 * addCachedSelectivityRangeVar
 *		add a new range clause for clauselist_selectivity
 *
 * Here is where we try to match up pairs of range-query clauses
 */
static void
addCachedSelectivityRangeVar(CachedSelectivityClause **cslist, Node *expr,
							 bool varonleft, bool isLTsel, Selectivity s2)
{
	CachedSelectivityClause *cselem;
	bool		is_lobound;

	is_lobound = (varonleft != isLTsel);

	cselem = findCachedSelectivityVar(cslist, expr);

	if (is_lobound)
	{
		if (!(cselem->selmask & CACHESEL_LOBOUND))
		{
			cselem->selmask |= CACHESEL_LOBOUND;
			cselem->lobound = s2;
		}
		else
		{
			/*------
			 * We have found two similar clauses, such as
			 * x < y AND x < z.
			 * Keep only the more restrictive one.
			 *------
			 */
			if (cselem->lobound > s2)
				cselem->lobound = s2;
		}
	}
	else
	{
		if (!(cselem->selmask & CACHESEL_HIBOUND))
		{
			cselem->selmask |= CACHESEL_HIBOUND;
			cselem->hibound = s2;
		}
		else
		{

			/*------
			 * We have found two similar clauses, such as
			 * x > y AND x > z.
			 * Keep only the more restrictive one.
			 *------
			 */
			if (cselem->hibound > s2)
				cselem->hibound = s2;
		}
	}
}

/*
 * addCachedSelectivityOtherStrictClause
 *		Cache the selectivity of other OpExpr type expressions which are
 *		strict
 */
static void
addCachedSelectivityOtherStrictClause(CachedSelectivityClause **cslist, Node *expr,
									  Selectivity s2)
{
	CachedSelectivityClause *cselem;

	cselem = findCachedSelectivityVar(cslist, expr);

	if ((cselem->selmask & CACHESEL_OTHERSTRICT))
		cselem->otherstrictsel = cselem->otherstrictsel * s2;
	else
	{
		cselem->otherstrictsel = s2;
		cselem->selmask |= CACHESEL_OTHERSTRICT;
	}
}

/*
 * find_relation_from_clauses
 *		Process each clause in 'clauses' and determine if all clauses
 *		reference only a single relation. If so return that relation,
 *		otherwise return NULL.
 */
static RelOptInfo *
find_relation_from_clauses(PlannerInfo *root, List *clauses)
{
	ListCell *l;
	int relid;
	int lastrelid = 0;

	foreach(l, clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		if (bms_get_singleton_member(rinfo->clause_relids, &relid))
		{
			if (lastrelid != 0 && relid != lastrelid)
				return NULL;		/* relation not the same as last one */
			lastrelid = relid;
		}
		else
			return NULL;			/* multiple relations in clause */
	}

	if (lastrelid != 0)
		return find_base_rel(root, lastrelid);

	return NULL;			/* no clauses */
}

/*
 * bms_is_subset_singleton
 *
 * Same result as bms_is_subset(s, bms_make_singleton(x)),
 * but a little faster and doesn't leak memory.
 *
 * Is this of use anywhere else?  If so move to bitmapset.c ...
 */
static bool
bms_is_subset_singleton(const Bitmapset *s, int x)
{
	switch (bms_membership(s))
	{
		case BMS_EMPTY_SET:
			return true;
		case BMS_SINGLETON:
			return bms_is_member(x, s);
		case BMS_MULTIPLE:
			return false;
	}
	/* can't get here... */
	return false;
}

/*
 * treat_as_join_clause -
 *	  Decide whether an operator clause is to be handled by the
 *	  restriction or join estimator.  Subroutine for clause_selectivity().
 */
static inline bool
treat_as_join_clause(Node *clause, RestrictInfo *rinfo,
					 int varRelid, SpecialJoinInfo *sjinfo)
{
	if (varRelid != 0)
	{
		/*
		 * Caller is forcing restriction mode (eg, because we are examining an
		 * inner indexscan qual).
		 */
		return false;
	}
	else if (sjinfo == NULL)
	{
		/*
		 * It must be a restriction clause, since it's being evaluated at a
		 * scan node.
		 */
		return false;
	}
	else
	{
		/*
		 * Otherwise, it's a join if there's more than one relation used. We
		 * can optimize this calculation if an rinfo was passed.
		 *
		 * XXX	Since we know the clause is being evaluated at a join, the
		 * only way it could be single-relation is if it was delayed by outer
		 * joins.  Although we can make use of the restriction qual estimators
		 * anyway, it seems likely that we ought to account for the
		 * probability of injected nulls somehow.
		 */
		if (rinfo)
			return (bms_membership(rinfo->clause_relids) == BMS_MULTIPLE);
		else
			return (NumRelids(clause) > 1);
	}
}


/*
 * clause_selectivity -
 *	  Compute the selectivity of a general boolean expression clause.
 *
 * The clause can be either a RestrictInfo or a plain expression.  If it's
 * a RestrictInfo, we try to cache the selectivity for possible re-use,
 * so passing RestrictInfos is preferred.
 *
 * varRelid is either 0 or a rangetable index.
 *
 * When varRelid is not 0, only variables belonging to that relation are
 * considered in computing selectivity; other vars are treated as constants
 * of unknown values.  This is appropriate for estimating the selectivity of
 * a join clause that is being used as a restriction clause in a scan of a
 * nestloop join's inner relation --- varRelid should then be the ID of the
 * inner relation.
 *
 * When varRelid is 0, all variables are treated as variables.  This
 * is appropriate for ordinary join clauses and restriction clauses.
 *
 * jointype is the join type, if the clause is a join clause.  Pass JOIN_INNER
 * if the clause isn't a join clause.
 *
 * sjinfo is NULL for a non-join clause, otherwise it provides additional
 * context information about the join being performed.  There are some
 * special cases:
 *	1. For a special (not INNER) join, sjinfo is always a member of
 *	   root->join_info_list.
 *	2. For an INNER join, sjinfo is just a transient struct, and only the
 *	   relids and jointype fields in it can be trusted.
 * It is possible for jointype to be different from sjinfo->jointype.
 * This indicates we are considering a variant join: either with
 * the LHS and RHS switched, or with one input unique-ified.
 *
 * Note: when passing nonzero varRelid, it's normally appropriate to set
 * jointype == JOIN_INNER, sjinfo == NULL, even if the clause is really a
 * join clause; because we aren't treating it as a join clause.
 */
Selectivity
clause_selectivity(PlannerInfo *root,
				   Node *clause,
				   int varRelid,
				   JoinType jointype,
				   SpecialJoinInfo *sjinfo)
{
	Selectivity s1 = 0.5;		/* default for any unhandled clause type */
	RestrictInfo *rinfo = NULL;
	bool		cacheable = false;

	if (clause == NULL)			/* can this still happen? */
		return s1;

	if (IsA(clause, RestrictInfo))
	{
		rinfo = (RestrictInfo *) clause;

		/*
		 * If the clause is marked pseudoconstant, then it will be used as a
		 * gating qual and should not affect selectivity estimates; hence
		 * return 1.0.  The only exception is that a constant FALSE may be
		 * taken as having selectivity 0.0, since it will surely mean no rows
		 * out of the plan.  This case is simple enough that we need not
		 * bother caching the result.
		 */
		if (rinfo->pseudoconstant)
		{
			if (!IsA(rinfo->clause, Const))
				return (Selectivity) 1.0;
		}

		/*
		 * If the clause is marked redundant, always return 1.0.
		 */
		if (rinfo->norm_selec > 1)
			return (Selectivity) 1.0;

		/*
		 * If possible, cache the result of the selectivity calculation for
		 * the clause.  We can cache if varRelid is zero or the clause
		 * contains only vars of that relid --- otherwise varRelid will affect
		 * the result, so mustn't cache.  Outer join quals might be examined
		 * with either their join's actual jointype or JOIN_INNER, so we need
		 * two cache variables to remember both cases.  Note: we assume the
		 * result won't change if we are switching the input relations or
		 * considering a unique-ified case, so we only need one cache variable
		 * for all non-JOIN_INNER cases.
		 */
		if (varRelid == 0 ||
			bms_is_subset_singleton(rinfo->clause_relids, varRelid))
		{
			/* Cacheable --- do we already have the result? */
			if (jointype == JOIN_INNER)
			{
				if (rinfo->norm_selec >= 0)
					return rinfo->norm_selec;
			}
			else
			{
				if (rinfo->outer_selec >= 0)
					return rinfo->outer_selec;
			}
			cacheable = true;
		}

		/*
		 * Proceed with examination of contained clause.  If the clause is an
		 * OR-clause, we want to look at the variant with sub-RestrictInfos,
		 * so that per-subclause selectivities can be cached.
		 */
		if (rinfo->orclause)
			clause = (Node *) rinfo->orclause;
		else
			clause = (Node *) rinfo->clause;
	}

	if (IsA(clause, Var))
	{
		Var		   *var = (Var *) clause;

		/*
		 * We probably shouldn't ever see an uplevel Var here, but if we do,
		 * return the default selectivity...
		 */
		if (var->varlevelsup == 0 &&
			(varRelid == 0 || varRelid == (int) var->varno))
		{
			/* Use the restriction selectivity function for a bool Var */
			s1 = boolvarsel(root, (Node *) var, varRelid);
		}
	}
	else if (IsA(clause, Const))
	{
		/* bool constant is pretty easy... */
		Const	   *con = (Const *) clause;

		s1 = con->constisnull ? 0.0 :
			DatumGetBool(con->constvalue) ? 1.0 : 0.0;
	}
	else if (IsA(clause, Param))
	{
		/* see if we can replace the Param */
		Node	   *subst = estimate_expression_value(root, clause);

		if (IsA(subst, Const))
		{
			/* bool constant is pretty easy... */
			Const	   *con = (Const *) subst;

			s1 = con->constisnull ? 0.0 :
				DatumGetBool(con->constvalue) ? 1.0 : 0.0;
		}
		else
		{
			/* XXX any way to do better than default? */
		}
	}
	else if (not_clause(clause))
	{
		/* inverse of the selectivity of the underlying clause */
		s1 = 1.0 - clause_selectivity(root,
								  (Node *) get_notclausearg((Expr *) clause),
									  varRelid,
									  jointype,
									  sjinfo);
	}
	else if (and_clause(clause))
	{
		/* share code with clauselist_selectivity() */
		s1 = clauselist_selectivity(root,
									((BoolExpr *) clause)->args,
									varRelid,
									jointype,
									sjinfo);
	}
	else if (or_clause(clause))
	{
		/*
		 * Selectivities for an OR clause are computed as s1+s2 - s1*s2 to
		 * account for the probable overlap of selected tuple sets.
		 *
		 * XXX is this too conservative?
		 */
		ListCell   *arg;

		s1 = 0.0;
		foreach(arg, ((BoolExpr *) clause)->args)
		{
			Selectivity s2 = clause_selectivity(root,
												(Node *) lfirst(arg),
												varRelid,
												jointype,
												sjinfo);

			s1 = s1 + s2 - s1 * s2;
		}
	}
	else if (is_opclause(clause) || IsA(clause, DistinctExpr))
	{
		OpExpr	   *opclause = (OpExpr *) clause;
		Oid			opno = opclause->opno;

		if (treat_as_join_clause(clause, rinfo, varRelid, sjinfo))
		{
			/* Estimate selectivity for a join clause. */
			s1 = join_selectivity(root, opno,
								  opclause->args,
								  opclause->inputcollid,
								  jointype,
								  sjinfo);
		}
		else
		{
			/* Estimate selectivity for a restriction clause. */
			s1 = restriction_selectivity(root, opno,
										 opclause->args,
										 opclause->inputcollid,
										 varRelid);
		}

		/*
		 * DistinctExpr has the same representation as OpExpr, but the
		 * contained operator is "=" not "<>", so we must negate the result.
		 * This estimation method doesn't give the right behavior for nulls,
		 * but it's better than doing nothing.
		 */
		if (IsA(clause, DistinctExpr))
			s1 = 1.0 - s1;
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		/* Use node specific selectivity calculation function */
		s1 = scalararraysel(root,
							(ScalarArrayOpExpr *) clause,
							treat_as_join_clause(clause, rinfo,
												 varRelid, sjinfo),
							varRelid,
							jointype,
							sjinfo);
	}
	else if (IsA(clause, RowCompareExpr))
	{
		/* Use node specific selectivity calculation function */
		s1 = rowcomparesel(root,
						   (RowCompareExpr *) clause,
						   varRelid,
						   jointype,
						   sjinfo);
	}
	else if (IsA(clause, NullTest))
	{
		/* Use node specific selectivity calculation function */
		s1 = nulltestsel(root,
						 ((NullTest *) clause)->nulltesttype,
						 (Node *) ((NullTest *) clause)->arg,
						 varRelid,
						 jointype,
						 sjinfo);
	}
	else if (IsA(clause, BooleanTest))
	{
		/* Use node specific selectivity calculation function */
		s1 = booltestsel(root,
						 ((BooleanTest *) clause)->booltesttype,
						 (Node *) ((BooleanTest *) clause)->arg,
						 varRelid,
						 jointype,
						 sjinfo);
	}
	else if (IsA(clause, CurrentOfExpr))
	{
		/* CURRENT OF selects at most one row of its table */
		CurrentOfExpr *cexpr = (CurrentOfExpr *) clause;
		RelOptInfo *crel = find_base_rel(root, cexpr->cvarno);

		if (crel->tuples > 0)
			s1 = 1.0 / crel->tuples;
	}
	else if (IsA(clause, RelabelType))
	{
		/* Not sure this case is needed, but it can't hurt */
		s1 = clause_selectivity(root,
								(Node *) ((RelabelType *) clause)->arg,
								varRelid,
								jointype,
								sjinfo);
	}
	else if (IsA(clause, CoerceToDomain))
	{
		/* Not sure this case is needed, but it can't hurt */
		s1 = clause_selectivity(root,
								(Node *) ((CoerceToDomain *) clause)->arg,
								varRelid,
								jointype,
								sjinfo);
	}
	else
	{
		/*
		 * For anything else, see if we can consider it as a boolean variable.
		 * This only works if it's an immutable expression in Vars of a single
		 * relation; but there's no point in us checking that here because
		 * boolvarsel() will do it internally, and return a suitable default
		 * selectivity if not.
		 */
		s1 = boolvarsel(root, clause, varRelid);
	}

	/* Cache the result if possible */
	if (cacheable)
	{
		if (jointype == JOIN_INNER)
			rinfo->norm_selec = s1;
		else
			rinfo->outer_selec = s1;
	}

#ifdef SELECTIVITY_DEBUG
	elog(DEBUG4, "clause_selectivity: s1 %f", s1);
#endif   /* SELECTIVITY_DEBUG */

	return s1;
}
