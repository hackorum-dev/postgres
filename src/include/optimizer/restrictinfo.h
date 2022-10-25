/*-------------------------------------------------------------------------
 *
 * restrictinfo.h
 *	  prototypes for restrictinfo.c.
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/restrictinfo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RESTRICTINFO_H
#define RESTRICTINFO_H

#include "nodes/pathnodes.h"


/* Convenience macro for the common case of a valid-everywhere qual */
#define make_simple_restrictinfo(root, clause)  \
	make_restrictinfo(root, clause, true, false, false, 0, NULL, NULL, NULL)

extern RestrictInfo *make_restrictinfo(PlannerInfo *root,
									   Expr *clause,
									   bool is_pushed_down,
									   bool outerjoin_delayed,
									   bool pseudoconstant,
									   Index security_level,
									   Relids required_relids,
									   Relids outer_relids,
									   Relids nullable_relids);
extern RestrictInfo *commute_restrictinfo(RestrictInfo *rinfo, Oid comm_op);
extern List *get_actual_clauses(List *restrictinfo_list);
extern List *extract_actual_clauses(List *restrictinfo_list,
									bool pseudoconstant);
extern void extract_actual_join_clauses(List *restrictinfo_list,
										Relids joinrelids,
										List **joinquals,
										List **otherquals);
extern bool join_clause_is_movable_to(RestrictInfo *rinfo, RelOptInfo *baserel);
extern bool join_clause_is_movable_into(RestrictInfo *rinfo,
										Relids currentrelids,
										Relids current_and_outer);

/*
 * restriction_is_or_clause
 *
 * Returns true iff the restrictinfo node contains an 'or' clause.
 */
static inline bool
restriction_is_or_clause(RestrictInfo *restrictinfo)
{
	return (restrictinfo->orclause != NULL);
}

/*
 * restriction_is_securely_promotable
 *
 * Returns true if it's okay to evaluate this clause "early", that is before
 * other restriction clauses attached to the specified relation.
 */
static inline bool
restriction_is_securely_promotable(RestrictInfo *restrictinfo,
								   RelOptInfo *rel)
{
	/*
	 * It's okay if there are no baserestrictinfo clauses for the rel that
	 * would need to go before this one, *or* if this one is leakproof.
	 */
	return (restrictinfo->security_level <= rel->baserestrict_min_security ||
			restrictinfo->leakproof);
}

#endif							/* RESTRICTINFO_H */
