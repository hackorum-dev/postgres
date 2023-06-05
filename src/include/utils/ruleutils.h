/*-------------------------------------------------------------------------
 *
 * ruleutils.h
 *		Declarations for ruleutils.c
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/ruleutils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RULEUTILS_H
#define RULEUTILS_H

#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "catalog/pg_sequence.h"

struct Plan;					/* avoid including plannodes.h here */
struct PlannedStmt;

/* Flags for pg_get_indexdef_columns_extended() */
#define RULE_INDEXDEF_PRETTY		0x01
#define RULE_INDEXDEF_KEYS_ONLY		0x02	/* ignore included attributes */

/*
 * IncludeSequenceDefaults decides on inclusion of DEFAULT clauses for columns
 * getting their default values from a sequence when creating the definition
 * of a table.
 */
typedef enum IncludeSequenceDefaults
{
	NO_SEQUENCE_DEFAULTS = 0,	/* don't include sequence defaults */
	INCLUDE_SEQUENCE_DEFAULTS = 1,	/* include sequence defaults */
} IncludeSequenceDefaults;

/*
 * IncludeIdentities decides on how we include identity information
 * when creating the definition of a table.
 */
typedef enum IncludeIdentities
{
	NO_IDENTITY = 0,			/* don't include identities */
	INCLUDE_IDENTITY = 1		/* include identities as-is */
} IncludeIdentities;

extern char *pg_get_indexdef_string(Oid indexrelid);
extern char *pg_get_indexdef_columns(Oid indexrelid, bool pretty);
extern char *pg_get_indexdef_columns_extended(Oid indexrelid,
											  bits16 flags);
extern char *pg_get_querydef(Query *query, bool pretty);

extern char *pg_get_partkeydef_columns(Oid relid, bool pretty);
extern char *pg_get_partconstrdef_string(Oid partitionId, char *aliasname);

extern char *pg_get_constraintdef_command(Oid constraintId);
extern char *deparse_expression(Node *expr, List *dpcontext,
								bool forceprefix, bool showimplicit);
extern List *deparse_context_for(const char *aliasname, Oid relid);
extern List *deparse_context_for_plan_tree(struct PlannedStmt *pstmt,
										   List *rtable_names);
extern List *set_deparse_context_plan(List *dpcontext,
									  struct Plan *plan, List *ancestors);
extern List *select_rtable_names_for_explain(List *rtable,
											 Bitmapset *rels_used);
extern char *generate_qualified_relation_name(Oid collid);
extern char *generate_collation_name(Oid collid);
extern char *generate_opclass_name(Oid opclass);
extern char *get_range_partbound_string(List *bound_datums);

extern char *pg_get_statisticsobjdef_string(Oid statextid);

/* Function declarations for version independent Citus ruleutils wrapper functions */
extern char *pg_get_extensiondef_string(Oid tableRelationId);
extern char *get_extension_version(Oid extensionId);
extern char *pg_get_serverdef_string(Oid tableRelationId);
extern char *pg_get_sequencedef_string(Oid sequenceRelid);
extern Form_pg_sequence pg_get_sequencedef(Oid sequenceRelationId);
extern char *pg_get_tableschemadef_string(Oid tableRelationId,
										  IncludeSequenceDefaults includeSequenceDefaults,
										  IncludeIdentities includeIdentityDefaults,
										  char *accessMethod);
extern char *pg_get_tablecolumnoptionsdef_string(Oid tableRelationId);
extern char *pg_get_indexclusterdef_string(Oid indexRelationId);
extern List *pg_get_table_grants(Oid relationId);
extern char *pg_get_replica_identity_command(Oid tableRelationId);
extern List *pg_get_row_level_security_commands(Oid relationId);
extern bool contain_nextval_expression_walker(Node *node, void *context);
extern void AppendOptionListToString(StringInfo stringData, List *options);
extern void AppendStorageParametersToString(StringInfo stringBuffer,
											List *optionList);

extern bool RegularTable(Oid relationId);
extern char *GeneratePartitioningInformation(Oid parentTableId);
extern bool PartitionedTable(Oid relationId);
extern const char *RoleSpecString(RoleSpec *spec, bool withQuoteIdentifier);
extern char *TableOwnerResetCommand(Oid relationId);
extern Oid	TableOwnerOid(Oid relationId);
extern char *TableOwner(Oid relationId);

#endif							/* RULEUTILS_H */
