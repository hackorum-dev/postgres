/*-------------------------------------------------------------------------
 *
 * parse_oper.h
 *		handle operator things for parser
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parse_oper.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_OPER_H
#define PARSE_OPER_H

#include "access/htup.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"


typedef HeapTuple Operator;

/*
 * An operator name List as produced by the grammar's OPERATOR() decoration
 * is EITHER a single possibly-qualified operator name (a List of Strings,
 * the traditional shape) OR a list of such names, one per column of a row
 * or subquery comparison (a List of such sub-lists).
 *
 * Does this operator name List carry multiple per-column names?
 */
#define OperatorNameIsList(opname) \
	((opname) != NIL && IsA(linitial((List *) (opname)), List))

/*
 * Reject a per-column operator name list (see OperatorNameIsList) where only a
 * single operator name is meaningful.  Pass a ParseState and location where
 * available so the error can point at the offending decoration; callers with
 * neither (e.g. DDL definition items) pass NULL and -1.
 */
extern void reject_operator_name_list(ParseState *pstate, List *opname,
									  int location);

/* Routines to look up an operator given name and exact input type(s) */
extern Oid	LookupOperName(ParseState *pstate, List *opername,
						   Oid oprleft, Oid oprright,
						   bool noError, int location);
extern Oid	LookupOperWithArgs(ObjectWithArgs *oper, bool noError);

/* Routines to find operators matching a name and given input types */
/* NB: the selected operator may require coercion of the input types! */
extern Operator oper(ParseState *pstate, List *opname, Oid ltypeId,
					 Oid rtypeId, bool noError, int location);
extern Operator left_oper(ParseState *pstate, List *op, Oid arg,
						  bool noError, int location);

/* Routines to find operators that DO NOT require coercion --- ie, their */
/* input types are either exactly as given, or binary-compatible */
extern Operator compatible_oper(ParseState *pstate, List *op,
								Oid arg1, Oid arg2,
								bool noError, int location);

/* currently no need for compatible_left_oper/compatible_right_oper */

/* Error reporting support */
extern const char *op_signature_string(List *op, Oid arg1, Oid arg2);

/* Routines for identifying "<", "=", ">" operators for a type */
extern void get_sort_group_operators(Oid argtype,
									 bool needLT, bool needEQ, bool needGT,
									 Oid *ltOpr, Oid *eqOpr, Oid *gtOpr,
									 bool *isHashable);

/* Convenience routines for common calls on the above */
extern Oid	compatible_oper_opid(List *op, Oid arg1, Oid arg2, bool noError);

/* Extract operator OID or underlying-function OID from an Operator tuple */
extern Oid	oprid(Operator op);
extern Oid	oprfuncid(Operator op);

/* Build expression tree for an operator invocation */
extern Expr *make_op(ParseState *pstate, List *opname,
					 Node *ltree, Node *rtree, Node *last_srf, int location);
extern Expr *make_scalar_array_op(ParseState *pstate, List *opname,
								  bool useOr,
								  Node *ltree, Node *rtree, int location);

#endif							/* PARSE_OPER_H */
