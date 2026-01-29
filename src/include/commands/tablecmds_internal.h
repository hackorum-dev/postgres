/*-------------------------------------------------------------------------
 *
 * tablecmds_internal.h
 *	  TODO
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/tablecmds_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLECMDS_INTERNAL_H
#define TABLECMDS_INTERNAL_H

#include "access/htup.h"
#include "access/tupdesc.h"
#include "nodes/parsenodes.h"
#include "storage/lockdefs.h"

/* to avoid including other headers */
typedef struct RelationData *Relation;
typedef struct List List;
typedef struct ObjectAddress ObjectAddress;
typedef struct ExprState ExprState;

/*
 * State information for ALTER TABLE
 *
 * The pending-work queue for an ALTER TABLE is a List of AlteredTableInfo
 * structs, one for each table modified by the operation (the named table
 * plus any child tables that are affected).  We save lists of subcommands
 * to apply to this table (possibly modified by parse transformation steps);
 * these lists will be executed in Phase 2.  If a Phase 3 step is needed,
 * necessary information is stored in the constraints and newvals lists.
 *
 * Phase 2 is divided into multiple passes; subcommands are executed in
 * a pass determined by subcommand type.
 */

typedef enum AlterTablePass
{
	AT_PASS_UNSET = -1,			/* UNSET will cause ERROR */
	AT_PASS_DROP,				/* DROP (all flavors) */
	AT_PASS_ALTER_TYPE,			/* ALTER COLUMN TYPE */
	AT_PASS_ADD_COL,			/* ADD COLUMN */
	AT_PASS_SET_EXPRESSION,		/* ALTER SET EXPRESSION */
	AT_PASS_OLD_INDEX,			/* re-add existing indexes */
	AT_PASS_OLD_CONSTR,			/* re-add existing constraints */
	/* We could support a RENAME COLUMN pass here, but not currently used */
	AT_PASS_ADD_CONSTR,			/* ADD constraints (initial examination) */
	AT_PASS_COL_ATTRS,			/* set column attributes, eg NOT NULL */
	AT_PASS_ADD_INDEXCONSTR,	/* ADD index-based constraints */
	AT_PASS_ADD_INDEX,			/* ADD indexes */
	AT_PASS_ADD_OTHERCONSTR,	/* ADD other constraints, defaults */
	AT_PASS_MISC,				/* other stuff */
} AlterTablePass;

#define AT_NUM_PASSES			(AT_PASS_MISC + 1)

typedef struct AlteredTableInfo
{
	/* Information saved before any work commences: */
	Oid			relid;			/* Relation to work on */
	char		relkind;		/* Its relkind */
	TupleDesc	oldDesc;		/* Pre-modification tuple descriptor */

	/*
	 * Transiently set during Phase 2, normally set to NULL.
	 *
	 * ATRewriteCatalogs sets this when it starts, and closes when ATExecCmd
	 * returns control.  This can be exploited by ATExecCmd subroutines to
	 * close/reopen across transaction boundaries.
	 */
	Relation	rel;

	/* Information saved by Phase 1 for Phase 2: */
	List	   *subcmds[AT_NUM_PASSES]; /* Lists of AlterTableCmd */
	/* Information saved by Phases 1/2 for Phase 3: */
	List	   *constraints;	/* List of NewConstraint */
	List	   *newvals;		/* List of NewColumnValue */
	List	   *afterStmts;		/* List of utility command parsetrees */
	bool		verify_new_notnull; /* T if we should recheck NOT NULL */
	int			rewrite;		/* Reason for forced rewrite, if any */
	bool		chgAccessMethod;	/* T if SET ACCESS METHOD is used */
	Oid			newAccessMethod;	/* new access method; 0 means no change,
									 * if above is true */
	Oid			newTableSpace;	/* new tablespace; 0 means no change */
	bool		chgPersistence; /* T if SET LOGGED/UNLOGGED is used */
	char		newrelpersistence;	/* if above is true */
	Expr	   *partition_constraint;	/* for attach partition validation */
	/* true, if validating default due to some other attach/detach */
	bool		validate_default;
	/* Objects to rebuild after completing ALTER TYPE operations */
	List	   *changedConstraintOids;	/* OIDs of constraints to rebuild */
	List	   *changedConstraintDefs;	/* string definitions of same */
	List	   *changedIndexOids;	/* OIDs of indexes to rebuild */
	List	   *changedIndexDefs;	/* string definitions of same */
	char	   *replicaIdentityIndex;	/* index to reset as REPLICA IDENTITY */
	char	   *clusterOnIndex; /* index to use for CLUSTER */
	List	   *changedStatisticsOids;	/* OIDs of statistics to rebuild */
	List	   *changedStatisticsDefs;	/* string definitions of same */
} AlteredTableInfo;

/* Struct describing one new constraint to check in Phase 3 scan */
/* Note: new not-null constraints are handled elsewhere */
typedef struct NewConstraint
{
	char	   *name;			/* Constraint name, or NULL if none */
	ConstrType	contype;		/* CHECK or FOREIGN */
	Oid			refrelid;		/* PK rel, if FOREIGN */
	Oid			refindid;		/* OID of PK's index, if FOREIGN */
	bool		conwithperiod;	/* Whether the new FOREIGN KEY uses PERIOD */
	Oid			conid;			/* OID of pg_constraint entry, if FOREIGN */
	Node	   *qual;			/* Check expr or CONSTR_FOREIGN Constraint */
	ExprState  *qualstate;		/* Execution state for CHECK expr */
} NewConstraint;

/*
 * Struct describing one new column value that needs to be computed during
 * Phase 3 copy (this could be either a new column with a non-null default, or
 * a column that we're changing the type of).  Columns without such an entry
 * are just copied from the old table during ATRewriteTable.  Note that the
 * expr is an expression over *old* table values, except when is_generated
 * is true; then it is an expression over columns of the *new* tuple.
 */
typedef struct NewColumnValue
{
	AttrNumber	attnum;			/* which column */
	Expr	   *expr;			/* expression to compute */
	ExprState  *exprstate;		/* execution state */
	bool		is_generated;	/* is it a GENERATED expression? */
} NewColumnValue;

/* Alter table target-type flags for ATSimplePermissions */
#define		ATT_TABLE				0x0001
#define		ATT_VIEW				0x0002
#define		ATT_MATVIEW				0x0004
#define		ATT_INDEX				0x0008
#define		ATT_COMPOSITE_TYPE		0x0010
#define		ATT_FOREIGN_TABLE		0x0020
#define		ATT_PARTITIONED_INDEX	0x0040
#define		ATT_SEQUENCE			0x0080
#define		ATT_PARTITIONED_TABLE	0x0100

/* Partial or complete FK creation in addFkConstraint() */
typedef enum addFkConstraintSides
{
	addFkReferencedSide,
	addFkReferencingSide,
	addFkBothSides,
} addFkConstraintSides;

extern void CheckAlterTableIsSafe(Relation rel);
extern void QueueFKConstraintValidation(List **wqueue, Relation conrel, Relation fkrel,
										Oid pkrelid, HeapTuple contuple, LOCKMODE lockmode);
extern AlteredTableInfo *ATGetQueueEntry(List **wqueue, Relation rel);
extern void ATSimplePermissions(AlterTableType cmdtype, Relation rel, int allowed_targets);
extern ObjectAddress ATExecDropIdentity(Relation rel, const char *colName, bool missing_ok, LOCKMODE lockmode,
										bool recurse, bool recursing);
extern ObjectAddress addFkConstraint(addFkConstraintSides fkside,
									 char *constraintname,
									 Constraint *fkconstraint, Relation rel,
									 Relation pkrel, Oid indexOid,
									 Oid parentConstr,
									 int numfks, int16 *pkattnum, int16 *fkattnum,
									 Oid *pfeqoperators, Oid *ppeqoperators,
									 Oid *ffeqoperators, int numfkdelsetcols,
									 int16 *fkdelsetcols, bool is_internal,
									 bool with_period);
extern void addFkRecurseReferenced(Constraint *fkconstraint,
								   Relation rel, Relation pkrel, Oid indexOid, Oid parentConstr,
								   int numfks, int16 *pkattnum, int16 *fkattnum,
								   Oid *pfeqoperators, Oid *ppeqoperators, Oid *ffeqoperators,
								   int numfkdelsetcols, int16 *fkdelsetcols,
								   bool old_check_ok,
								   Oid parentDelTrigger, Oid parentUpdTrigger,
								   bool with_period);
extern void addFkRecurseReferencing(List **wqueue, Constraint *fkconstraint,
									Relation rel, Relation pkrel, Oid indexOid, Oid parentConstr,
									int numfks, int16 *pkattnum, int16 *fkattnum,
									Oid *pfeqoperators, Oid *ppeqoperators, Oid *ffeqoperators,
									int numfkdelsetcols, int16 *fkdelsetcols,
									bool old_check_ok, LOCKMODE lockmode,
									Oid parentInsTrigger, Oid parentUpdTrigger,
									bool with_period);
extern void DropForeignKeyConstraintTriggers(Relation trigrel, Oid conoid,
											 Oid confrelid, Oid conrelid);
extern void CreateInheritance(Relation child_rel, Relation parent_rel, bool ispartition);
extern void RemoveInheritance(Relation child_rel, Relation parent_rel,
							  bool expect_detached);

#endif							/* TABLECMDS_INTERNAL_H */
