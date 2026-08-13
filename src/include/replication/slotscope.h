/*-------------------------------------------------------------------------
 *
 * slotscope.h
 *	  Logical replication slot scope management.
 *
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *	  src/include/replication/slotscope.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SLOTSCOPE_H
#define SLOTSCOPE_H

#include "nodes/pg_list.h"

struct RelationData;
struct ReplicationSlot;

extern void LogicalSlotScopeInitialize(void);
extern void LogicalSlotScopeReconcileDatabase(void);
extern void LogicalSlotScopeConfigureUnrestricted(struct ReplicationSlot *slot);

/* Caller must provide a transaction environment. */
extern void LogicalSlotScopePrepareFromPublications(
													struct ReplicationSlot *slot, List *pubnames);
extern void LogicalSlotScopePrepareFromPublicationOids(
													   struct ReplicationSlot *slot, List *publications);
extern void LogicalSlotScopeFinishCreate(struct ReplicationSlot *slot,
										 bool finalize_at_xact_end);
extern void LogicalSlotScopeRestorePublications(struct ReplicationSlot *slot,
												List *publications,
												uint64 incarnation,
												XLogRecPtr ready_lsn);
extern void LogicalSlotScopeRestore(struct ReplicationSlot *slot);
extern List *LogicalSlotScopeGetPublications(struct ReplicationSlot *slot);
extern void LogicalSlotScopePublicationAddRelations(Oid pubid, List *relations);
extern void LogicalSlotScopeNoteToastCreation(Oid owner, Oid toastrelid);
extern void LogicalSlotScopeRelationDrop(Oid relid);
extern void LogicalSlotScopeCleanup(void);
extern bool LogicalSlotScopePublicationsContain(struct ReplicationSlot *slot,
												List *pubnames);

extern void CheckLogicalSlotScopeHierarchyChange(Oid childrelid,
												 Oid parentrelid);
extern bool RelationNeedsLogicalTupleWAL(struct RelationData *relation);

#endif							/* SLOTSCOPE_H */
