/*-------------------------------------------------------------------------
 *
 * tablecmds_partition.h
 *	  prototypes for tablecmds_partition.c.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/tablecmds_partition.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLECMDS_PARTITION_H
#define TABLECMDS_PARTITION_H

#include "access/attnum.h"
#include "nodes/parsenodes.h"
#include "storage/lockdefs.h"

/* to avoid including other headers */
typedef struct AlteredTableInfo AlteredTableInfo;
typedef struct ParseState ParseState;
typedef struct AlterTableUtilityContext AlterTableUtilityContext;
typedef struct ForeignKeyCacheInfo ForeignKeyCacheInfo;
typedef struct RelationData *Relation;
typedef struct List List;
typedef struct ObjectAddress ObjectAddress;

extern void ATCheckPartitionsNotInUse(Relation rel, LOCKMODE lockmode);
extern bool ConstraintImpliedByRelConstraint(Relation scanrel,
											 List *testConstraint, List *provenConstraint);
extern void CloneForeignKeyConstraints(List **wqueue, Relation parentRel,
									   Relation partitionRel);
extern bool tryAttachPartitionForeignKey(List **wqueue,
										 ForeignKeyCacheInfo *fk,
										 Relation partition,
										 Oid parentConstrOid, int numfks,
										 AttrNumber *mapped_conkey, AttrNumber *confkey,
										 Oid *conpfeqop,
										 Oid parentInsTrigger,
										 Oid parentUpdTrigger,
										 Relation trigrel);
extern PartitionSpec *transformPartitionSpec(Relation rel, PartitionSpec *partspec);
extern void ComputePartitionAttrs(ParseState *pstate, Relation rel, List *partParams, AttrNumber *partattrs,
								  List **partexprs, Oid *partopclass, Oid *partcollation,
								  PartitionStrategy strategy);
extern bool PartConstraintImpliedByRelConstraint(Relation scanrel,
												 List *partConstraint);
extern ObjectAddress ATExecAttachPartition(List **wqueue, Relation rel,
										   PartitionCmd *cmd,
										   AlterTableUtilityContext *context);
extern void CloneRowTriggersToPartition(Relation parent, Relation partition);
extern ObjectAddress ATExecDetachPartition(List **wqueue, AlteredTableInfo *tab,
										   Relation rel, RangeVar *name,
										   bool concurrent);
extern ObjectAddress ATExecDetachPartitionFinalize(Relation rel, RangeVar *name);
extern ObjectAddress ATExecAttachPartitionIdx(List **wqueue, Relation parentIdx,
											  RangeVar *name);
extern void ATExecMergePartitions(List **wqueue, AlteredTableInfo *tab, Relation rel,
								  PartitionCmd *cmd, AlterTableUtilityContext *context);
extern void ATExecSplitPartition(List **wqueue, AlteredTableInfo *tab,
								 Relation rel, PartitionCmd *cmd,
								 AlterTableUtilityContext *context);

#endif							/* TABLECMDS_PARTITION_H */
