/*-------------------------------------------------------------------------
 *
 * tablecmds_partition.c
 *	  TODO
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/tablecmds_partition.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/toast_compression.h"
#include "utils/lsyscache.h"
#include "access/relation.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_trigger.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/tablecmds_internal.h"
#include "commands/tablecmds_partition.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "parser/parse_utilcmd.h"
#include "partitioning/partbounds.h"
#include "partitioning/partdesc.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static void CloneFkReferenced(Relation parentRel, Relation partitionRel);
static void CloneFkReferencing(List **wqueue, Relation parentRel,
							   Relation partRel);
static void AttachPartitionForeignKey(List **wqueue, Relation partition,
									  Oid partConstrOid, Oid parentConstrOid,
									  Oid parentInsTrigger, Oid parentUpdTrigger,
									  Relation trigrel);
static void RemoveInheritedConstraint(Relation conrel, Relation trigrel,
									  Oid conoid, Oid conrelid);
static void GetForeignKeyActionTriggers(Relation trigrel,
										Oid conoid, Oid confrelid, Oid conrelid,
										Oid *deleteTriggerOid,
										Oid *updateTriggerOid);
static void GetForeignKeyCheckTriggers(Relation trigrel,
									   Oid conoid, Oid confrelid, Oid conrelid,
									   Oid *insertTriggerOid,
									   Oid *updateTriggerOid);
static void AttachPartitionEnsureIndexes(List **wqueue, Relation rel, Relation attachrel);
static void QueuePartitionConstraintValidation(List **wqueue, Relation scanrel,
											   List *partConstraint,
											   bool validate_default);
static void DropClonedTriggersFromPartition(Oid partitionId);
static void DetachPartitionFinalize(Relation rel, Relation partRel,
									bool concurrent, Oid defaultPartOid);
static void validatePartitionedIndex(Relation partedIdx, Relation partedTbl);
static void refuseDupeIndexAttach(Relation parentIdx, Relation partIdx,
								  Relation partitionTbl);
static void verifyPartitionIndexNotNull(IndexInfo *iinfo, Relation partition);
static List *GetParentedForeignKeyRefs(Relation partition);
static void ATDetachCheckNoForeignKeyRefs(Relation partition);

/*
 * Obtain list of partitions of the given table, locking them all at the given
 * lockmode and ensuring that they all pass CheckAlterTableIsSafe.
 *
 * This function is a no-op if the given relation is not a partitioned table;
 * in particular, nothing is done if it's a legacy inheritance parent.
 */
void
ATCheckPartitionsNotInUse(Relation rel, LOCKMODE lockmode)
{
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		List	   *inh;
		ListCell   *cell;

		inh = find_all_inheritors(RelationGetRelid(rel), lockmode, NULL);
		/* first element is the parent rel; must ignore it */
		for_each_from(cell, inh, 1)
		{
			Relation	childrel;

			/* find_all_inheritors already got lock */
			childrel = table_open(lfirst_oid(cell), NoLock);
			CheckAlterTableIsSafe(childrel);
			table_close(childrel, NoLock);
		}
		list_free(inh);
	}
}

/*
 * CloneForeignKeyConstraints
 *		Clone foreign keys from a partitioned table to a newly acquired
 *		partition.
 *
 * partitionRel is a partition of parentRel, so we can be certain that it has
 * the same columns with the same datatypes.  The columns may be in different
 * order, though.
 *
 * wqueue must be passed to set up phase 3 constraint checking, unless the
 * referencing-side partition is known to be empty (such as in CREATE TABLE /
 * PARTITION OF).
 */
void
CloneForeignKeyConstraints(List **wqueue, Relation parentRel,
						   Relation partitionRel)
{
	/* This only works for declarative partitioning */
	Assert(parentRel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	/*
	 * First, clone constraints where the parent is on the referencing side.
	 */
	CloneFkReferencing(wqueue, parentRel, partitionRel);

	/*
	 * Clone constraints for which the parent is on the referenced side.
	 */
	CloneFkReferenced(parentRel, partitionRel);
}

/*
 * CloneFkReferenced
 *		Subroutine for CloneForeignKeyConstraints
 *
 * Find all the FKs that have the parent relation on the referenced side;
 * clone those constraints to the given partition.  This is to be called
 * when the partition is being created or attached.
 *
 * This recurses to partitions, if the relation being attached is partitioned.
 * Recursion is done by calling addFkRecurseReferenced.
 */
static void
CloneFkReferenced(Relation parentRel, Relation partitionRel)
{
	Relation	pg_constraint;
	AttrMap    *attmap;
	ListCell   *cell;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tuple;
	List	   *clone = NIL;
	Relation	trigrel;

	/*
	 * Search for any constraints where this partition's parent is in the
	 * referenced side.  However, we must not clone any constraint whose
	 * parent constraint is also going to be cloned, to avoid duplicates.  So
	 * do it in two steps: first construct the list of constraints to clone,
	 * then go over that list cloning those whose parents are not in the list.
	 * (We must not rely on the parent being seen first, since the catalog
	 * scan could return children first.)
	 */
	pg_constraint = table_open(ConstraintRelationId, RowShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_constraint_confrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(parentRel)));
	ScanKeyInit(&key[1],
				Anum_pg_constraint_contype, BTEqualStrategyNumber,
				F_CHAREQ, CharGetDatum(CONSTRAINT_FOREIGN));
	/* This is a seqscan, as we don't have a usable index ... */
	scan = systable_beginscan(pg_constraint, InvalidOid, true,
							  NULL, 2, key);
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		clone = lappend_oid(clone, constrForm->oid);
	}
	systable_endscan(scan);
	table_close(pg_constraint, RowShareLock);

	/*
	 * Triggers of the foreign keys will be manipulated a bunch of times in
	 * the loop below.  To avoid repeatedly opening/closing the trigger
	 * catalog relation, we open it here and pass it to the subroutines called
	 * below.
	 */
	trigrel = table_open(TriggerRelationId, RowExclusiveLock);

	attmap = build_attrmap_by_name(RelationGetDescr(partitionRel),
								   RelationGetDescr(parentRel),
								   false);
	foreach(cell, clone)
	{
		Oid			constrOid = lfirst_oid(cell);
		Form_pg_constraint constrForm;
		Relation	fkRel;
		Oid			indexOid;
		Oid			partIndexId;
		int			numfks;
		AttrNumber	conkey[INDEX_MAX_KEYS];
		AttrNumber	mapped_confkey[INDEX_MAX_KEYS];
		AttrNumber	confkey[INDEX_MAX_KEYS];
		Oid			conpfeqop[INDEX_MAX_KEYS];
		Oid			conppeqop[INDEX_MAX_KEYS];
		Oid			conffeqop[INDEX_MAX_KEYS];
		int			numfkdelsetcols;
		AttrNumber	confdelsetcols[INDEX_MAX_KEYS];
		Constraint *fkconstraint;
		ObjectAddress address;
		Oid			deleteTriggerOid = InvalidOid,
					updateTriggerOid = InvalidOid;

		tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(constrOid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for constraint %u", constrOid);
		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/*
		 * As explained above: don't try to clone a constraint for which we're
		 * going to clone the parent.
		 */
		if (list_member_oid(clone, constrForm->conparentid))
		{
			ReleaseSysCache(tuple);
			continue;
		}

		/* We need the same lock level that CreateTrigger will acquire */
		fkRel = table_open(constrForm->conrelid, ShareRowExclusiveLock);

		indexOid = constrForm->conindid;
		DeconstructFkConstraintRow(tuple,
								   &numfks,
								   conkey,
								   confkey,
								   conpfeqop,
								   conppeqop,
								   conffeqop,
								   &numfkdelsetcols,
								   confdelsetcols);

		for (int i = 0; i < numfks; i++)
			mapped_confkey[i] = attmap->attnums[confkey[i] - 1];

		fkconstraint = makeNode(Constraint);
		fkconstraint->contype = CONSTRAINT_FOREIGN;
		fkconstraint->conname = NameStr(constrForm->conname);
		fkconstraint->deferrable = constrForm->condeferrable;
		fkconstraint->initdeferred = constrForm->condeferred;
		fkconstraint->location = -1;
		fkconstraint->pktable = NULL;
		/* ->fk_attrs determined below */
		fkconstraint->pk_attrs = NIL;
		fkconstraint->fk_matchtype = constrForm->confmatchtype;
		fkconstraint->fk_upd_action = constrForm->confupdtype;
		fkconstraint->fk_del_action = constrForm->confdeltype;
		fkconstraint->fk_del_set_cols = NIL;
		fkconstraint->old_conpfeqop = NIL;
		fkconstraint->old_pktable_oid = InvalidOid;
		fkconstraint->is_enforced = constrForm->conenforced;
		fkconstraint->skip_validation = false;
		fkconstraint->initially_valid = constrForm->convalidated;

		/* set up colnames that are used to generate the constraint name */
		for (int i = 0; i < numfks; i++)
		{
			Form_pg_attribute att;

			att = TupleDescAttr(RelationGetDescr(fkRel),
								conkey[i] - 1);
			fkconstraint->fk_attrs = lappend(fkconstraint->fk_attrs,
											 makeString(NameStr(att->attname)));
		}

		/*
		 * Add the new foreign key constraint pointing to the new partition.
		 * Because this new partition appears in the referenced side of the
		 * constraint, we don't need to set up for Phase 3 check.
		 */
		partIndexId = index_get_partition(partitionRel, indexOid);
		if (!OidIsValid(partIndexId))
			elog(ERROR, "index for %u not found in partition %s",
				 indexOid, RelationGetRelationName(partitionRel));

		/*
		 * Get the "action" triggers belonging to the constraint to pass as
		 * parent OIDs for similar triggers that will be created on the
		 * partition in addFkRecurseReferenced().
		 */
		if (constrForm->conenforced)
			GetForeignKeyActionTriggers(trigrel, constrOid,
										constrForm->confrelid, constrForm->conrelid,
										&deleteTriggerOid, &updateTriggerOid);

		/* Add this constraint ... */
		address = addFkConstraint(addFkReferencedSide,
								  fkconstraint->conname, fkconstraint, fkRel,
								  partitionRel, partIndexId, constrOid,
								  numfks, mapped_confkey,
								  conkey, conpfeqop, conppeqop, conffeqop,
								  numfkdelsetcols, confdelsetcols, false,
								  constrForm->conperiod);
		/* ... and recurse */
		addFkRecurseReferenced(fkconstraint,
							   fkRel,
							   partitionRel,
							   partIndexId,
							   address.objectId,
							   numfks,
							   mapped_confkey,
							   conkey,
							   conpfeqop,
							   conppeqop,
							   conffeqop,
							   numfkdelsetcols,
							   confdelsetcols,
							   true,
							   deleteTriggerOid,
							   updateTriggerOid,
							   constrForm->conperiod);

		table_close(fkRel, NoLock);
		ReleaseSysCache(tuple);
	}

	table_close(trigrel, RowExclusiveLock);
}

/*
 * CloneFkReferencing
 *		Subroutine for CloneForeignKeyConstraints
 *
 * For each FK constraint of the parent relation in the given list, find an
 * equivalent constraint in its partition relation that can be reparented;
 * if one cannot be found, create a new constraint in the partition as its
 * child.
 *
 * If wqueue is given, it is used to set up phase-3 verification for each
 * cloned constraint; omit it if such verification is not needed
 * (example: the partition is being created anew).
 */
static void
CloneFkReferencing(List **wqueue, Relation parentRel, Relation partRel)
{
	AttrMap    *attmap;
	List	   *partFKs;
	List	   *clone = NIL;
	ListCell   *cell;
	Relation	trigrel;

	/* obtain a list of constraints that we need to clone */
	foreach(cell, RelationGetFKeyList(parentRel))
	{
		ForeignKeyCacheInfo *fk = lfirst(cell);

		/*
		 * Refuse to attach a table as partition that this partitioned table
		 * already has a foreign key to.  This isn't useful schema, which is
		 * proven by the fact that there have been no user complaints that
		 * it's already impossible to achieve this in the opposite direction,
		 * i.e., creating a foreign key that references a partition.  This
		 * restriction allows us to dodge some complexities around
		 * pg_constraint and pg_trigger row creations that would be needed
		 * during ATTACH/DETACH for this kind of relationship.
		 */
		if (fk->confrelid == RelationGetRelid(partRel))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot attach table \"%s\" as a partition because it is referenced by foreign key \"%s\"",
							RelationGetRelationName(partRel),
							get_constraint_name(fk->conoid))));

		clone = lappend_oid(clone, fk->conoid);
	}

	/*
	 * Silently do nothing if there's nothing to do.  In particular, this
	 * avoids throwing a spurious error for foreign tables.
	 */
	if (clone == NIL)
		return;

	if (partRel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("foreign key constraints are not supported on foreign tables")));

	/*
	 * Triggers of the foreign keys will be manipulated a bunch of times in
	 * the loop below.  To avoid repeatedly opening/closing the trigger
	 * catalog relation, we open it here and pass it to the subroutines called
	 * below.
	 */
	trigrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * The constraint key may differ, if the columns in the partition are
	 * different.  This map is used to convert them.
	 */
	attmap = build_attrmap_by_name(RelationGetDescr(partRel),
								   RelationGetDescr(parentRel),
								   false);

	partFKs = copyObject(RelationGetFKeyList(partRel));

	foreach(cell, clone)
	{
		Oid			parentConstrOid = lfirst_oid(cell);
		Form_pg_constraint constrForm;
		Relation	pkrel;
		HeapTuple	tuple;
		int			numfks;
		AttrNumber	conkey[INDEX_MAX_KEYS];
		AttrNumber	mapped_conkey[INDEX_MAX_KEYS];
		AttrNumber	confkey[INDEX_MAX_KEYS];
		Oid			conpfeqop[INDEX_MAX_KEYS];
		Oid			conppeqop[INDEX_MAX_KEYS];
		Oid			conffeqop[INDEX_MAX_KEYS];
		int			numfkdelsetcols;
		AttrNumber	confdelsetcols[INDEX_MAX_KEYS];
		Constraint *fkconstraint;
		bool		attached;
		Oid			indexOid;
		ObjectAddress address;
		ListCell   *lc;
		Oid			insertTriggerOid = InvalidOid,
					updateTriggerOid = InvalidOid;
		bool		with_period;

		tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(parentConstrOid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for constraint %u",
				 parentConstrOid);
		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/* Don't clone constraints whose parents are being cloned */
		if (list_member_oid(clone, constrForm->conparentid))
		{
			ReleaseSysCache(tuple);
			continue;
		}

		/*
		 * Need to prevent concurrent deletions.  If pkrel is a partitioned
		 * relation, that means to lock all partitions.
		 */
		pkrel = table_open(constrForm->confrelid, ShareRowExclusiveLock);
		if (pkrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			(void) find_all_inheritors(RelationGetRelid(pkrel),
									   ShareRowExclusiveLock, NULL);

		DeconstructFkConstraintRow(tuple, &numfks, conkey, confkey,
								   conpfeqop, conppeqop, conffeqop,
								   &numfkdelsetcols, confdelsetcols);
		for (int i = 0; i < numfks; i++)
			mapped_conkey[i] = attmap->attnums[conkey[i] - 1];

		/*
		 * Get the "check" triggers belonging to the constraint, if it is
		 * ENFORCED, to pass as parent OIDs for similar triggers that will be
		 * created on the partition in addFkRecurseReferencing().  They are
		 * also passed to tryAttachPartitionForeignKey() below to simply
		 * assign as parents to the partition's existing "check" triggers,
		 * that is, if the corresponding constraints is deemed attachable to
		 * the parent constraint.
		 */
		if (constrForm->conenforced)
			GetForeignKeyCheckTriggers(trigrel, constrForm->oid,
									   constrForm->confrelid, constrForm->conrelid,
									   &insertTriggerOid, &updateTriggerOid);

		/*
		 * Before creating a new constraint, see whether any existing FKs are
		 * fit for the purpose.  If one is, attach the parent constraint to
		 * it, and don't clone anything.  This way we avoid the expensive
		 * verification step and don't end up with a duplicate FK, and we
		 * don't need to recurse to partitions for this constraint.
		 */
		attached = false;
		foreach(lc, partFKs)
		{
			ForeignKeyCacheInfo *fk = lfirst_node(ForeignKeyCacheInfo, lc);

			if (tryAttachPartitionForeignKey(wqueue,
											 fk,
											 partRel,
											 parentConstrOid,
											 numfks,
											 mapped_conkey,
											 confkey,
											 conpfeqop,
											 insertTriggerOid,
											 updateTriggerOid,
											 trigrel))
			{
				attached = true;
				table_close(pkrel, NoLock);
				break;
			}
		}
		if (attached)
		{
			ReleaseSysCache(tuple);
			continue;
		}

		/* No dice.  Set up to create our own constraint */
		fkconstraint = makeNode(Constraint);
		fkconstraint->contype = CONSTRAINT_FOREIGN;
		/* ->conname determined below */
		fkconstraint->deferrable = constrForm->condeferrable;
		fkconstraint->initdeferred = constrForm->condeferred;
		fkconstraint->location = -1;
		fkconstraint->pktable = NULL;
		/* ->fk_attrs determined below */
		fkconstraint->pk_attrs = NIL;
		fkconstraint->fk_matchtype = constrForm->confmatchtype;
		fkconstraint->fk_upd_action = constrForm->confupdtype;
		fkconstraint->fk_del_action = constrForm->confdeltype;
		fkconstraint->fk_del_set_cols = NIL;
		fkconstraint->old_conpfeqop = NIL;
		fkconstraint->old_pktable_oid = InvalidOid;
		fkconstraint->is_enforced = constrForm->conenforced;
		fkconstraint->skip_validation = false;
		fkconstraint->initially_valid = constrForm->convalidated;
		for (int i = 0; i < numfks; i++)
		{
			Form_pg_attribute att;

			att = TupleDescAttr(RelationGetDescr(partRel),
								mapped_conkey[i] - 1);
			fkconstraint->fk_attrs = lappend(fkconstraint->fk_attrs,
											 makeString(NameStr(att->attname)));
		}

		indexOid = constrForm->conindid;
		with_period = constrForm->conperiod;

		/* Create the pg_constraint entry at this level */
		address = addFkConstraint(addFkReferencingSide,
								  NameStr(constrForm->conname), fkconstraint,
								  partRel, pkrel, indexOid, parentConstrOid,
								  numfks, confkey,
								  mapped_conkey, conpfeqop,
								  conppeqop, conffeqop,
								  numfkdelsetcols, confdelsetcols,
								  false, with_period);

		/* Done with the cloned constraint's tuple */
		ReleaseSysCache(tuple);

		/* Create the check triggers, and recurse to partitions, if any */
		addFkRecurseReferencing(wqueue,
								fkconstraint,
								partRel,
								pkrel,
								indexOid,
								address.objectId,
								numfks,
								confkey,
								mapped_conkey,
								conpfeqop,
								conppeqop,
								conffeqop,
								numfkdelsetcols,
								confdelsetcols,
								false,	/* no old check exists */
								AccessExclusiveLock,
								insertTriggerOid,
								updateTriggerOid,
								with_period);
		table_close(pkrel, NoLock);
	}

	table_close(trigrel, RowExclusiveLock);
}

/*
 * When the parent of a partition receives [the referencing side of] a foreign
 * key, we must propagate that foreign key to the partition.  However, the
 * partition might already have an equivalent foreign key; this routine
 * compares the given ForeignKeyCacheInfo (in the partition) to the FK defined
 * by the other parameters.  If they are equivalent, create the link between
 * the two constraints and return true.
 *
 * If the given FK does not match the one defined by rest of the params,
 * return false.
 */
bool
tryAttachPartitionForeignKey(List **wqueue,
							 ForeignKeyCacheInfo *fk,
							 Relation partition,
							 Oid parentConstrOid,
							 int numfks,
							 AttrNumber *mapped_conkey,
							 AttrNumber *confkey,
							 Oid *conpfeqop,
							 Oid parentInsTrigger,
							 Oid parentUpdTrigger,
							 Relation trigrel)
{
	HeapTuple	parentConstrTup;
	Form_pg_constraint parentConstr;
	HeapTuple	partcontup;
	Form_pg_constraint partConstr;

	parentConstrTup = SearchSysCache1(CONSTROID,
									  ObjectIdGetDatum(parentConstrOid));
	if (!HeapTupleIsValid(parentConstrTup))
		elog(ERROR, "cache lookup failed for constraint %u", parentConstrOid);
	parentConstr = (Form_pg_constraint) GETSTRUCT(parentConstrTup);

	/*
	 * Do some quick & easy initial checks.  If any of these fail, we cannot
	 * use this constraint.
	 */
	if (fk->confrelid != parentConstr->confrelid || fk->nkeys != numfks)
	{
		ReleaseSysCache(parentConstrTup);
		return false;
	}
	for (int i = 0; i < numfks; i++)
	{
		if (fk->conkey[i] != mapped_conkey[i] ||
			fk->confkey[i] != confkey[i] ||
			fk->conpfeqop[i] != conpfeqop[i])
		{
			ReleaseSysCache(parentConstrTup);
			return false;
		}
	}

	/* Looks good so far; perform more extensive checks. */
	partcontup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(fk->conoid));
	if (!HeapTupleIsValid(partcontup))
		elog(ERROR, "cache lookup failed for constraint %u", fk->conoid);
	partConstr = (Form_pg_constraint) GETSTRUCT(partcontup);

	/*
	 * An error should be raised if the constraint enforceability is
	 * different. Returning false without raising an error, as we do for other
	 * attributes, could lead to a duplicate constraint with the same
	 * enforceability as the parent. While this may be acceptable, it may not
	 * be ideal. Therefore, it's better to raise an error and allow the user
	 * to correct the enforceability before proceeding.
	 */
	if (partConstr->conenforced != parentConstr->conenforced)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("constraint \"%s\" enforceability conflicts with constraint \"%s\" on relation \"%s\"",
						NameStr(parentConstr->conname),
						NameStr(partConstr->conname),
						RelationGetRelationName(partition))));

	if (OidIsValid(partConstr->conparentid) ||
		partConstr->condeferrable != parentConstr->condeferrable ||
		partConstr->condeferred != parentConstr->condeferred ||
		partConstr->confupdtype != parentConstr->confupdtype ||
		partConstr->confdeltype != parentConstr->confdeltype ||
		partConstr->confmatchtype != parentConstr->confmatchtype)
	{
		ReleaseSysCache(parentConstrTup);
		ReleaseSysCache(partcontup);
		return false;
	}

	ReleaseSysCache(parentConstrTup);
	ReleaseSysCache(partcontup);

	/* Looks good!  Attach this constraint. */
	AttachPartitionForeignKey(wqueue, partition, fk->conoid,
							  parentConstrOid, parentInsTrigger,
							  parentUpdTrigger, trigrel);

	return true;
}

/*
 * AttachPartitionForeignKey
 *
 * The subroutine for tryAttachPartitionForeignKey performs the final tasks of
 * attaching the constraint, removing redundant triggers and entries from
 * pg_constraint, and setting the constraint's parent.
 */
static void
AttachPartitionForeignKey(List **wqueue,
						  Relation partition,
						  Oid partConstrOid,
						  Oid parentConstrOid,
						  Oid parentInsTrigger,
						  Oid parentUpdTrigger,
						  Relation trigrel)
{
	HeapTuple	parentConstrTup;
	Form_pg_constraint parentConstr;
	HeapTuple	partcontup;
	Form_pg_constraint partConstr;
	bool		queueValidation;
	Oid			partConstrFrelid;
	Oid			partConstrRelid;
	bool		parentConstrIsEnforced;

	/* Fetch the parent constraint tuple */
	parentConstrTup = SearchSysCache1(CONSTROID,
									  ObjectIdGetDatum(parentConstrOid));
	if (!HeapTupleIsValid(parentConstrTup))
		elog(ERROR, "cache lookup failed for constraint %u", parentConstrOid);
	parentConstr = (Form_pg_constraint) GETSTRUCT(parentConstrTup);
	parentConstrIsEnforced = parentConstr->conenforced;

	/* Fetch the child constraint tuple */
	partcontup = SearchSysCache1(CONSTROID,
								 ObjectIdGetDatum(partConstrOid));
	if (!HeapTupleIsValid(partcontup))
		elog(ERROR, "cache lookup failed for constraint %u", partConstrOid);
	partConstr = (Form_pg_constraint) GETSTRUCT(partcontup);
	partConstrFrelid = partConstr->confrelid;
	partConstrRelid = partConstr->conrelid;

	/*
	 * If the referenced table is partitioned, then the partition we're
	 * attaching now has extra pg_constraint rows and action triggers that are
	 * no longer needed.  Remove those.
	 */
	if (get_rel_relkind(partConstrFrelid) == RELKIND_PARTITIONED_TABLE)
	{
		Relation	pg_constraint = table_open(ConstraintRelationId, RowShareLock);

		RemoveInheritedConstraint(pg_constraint, trigrel, partConstrOid,
								  partConstrRelid);

		table_close(pg_constraint, RowShareLock);
	}

	/*
	 * Will we need to validate this constraint?   A valid parent constraint
	 * implies that all child constraints have been validated, so if this one
	 * isn't, we must trigger phase 3 validation.
	 */
	queueValidation = parentConstr->convalidated && !partConstr->convalidated;

	ReleaseSysCache(partcontup);
	ReleaseSysCache(parentConstrTup);

	/*
	 * The action triggers in the new partition become redundant -- the parent
	 * table already has equivalent ones, and those will be able to reach the
	 * partition.  Remove the ones in the partition.  We identify them because
	 * they have our constraint OID, as well as being on the referenced rel.
	 */
	DropForeignKeyConstraintTriggers(trigrel, partConstrOid, partConstrFrelid,
									 partConstrRelid);

	ConstraintSetParentConstraint(partConstrOid, parentConstrOid,
								  RelationGetRelid(partition));

	/*
	 * Like the constraint, attach partition's "check" triggers to the
	 * corresponding parent triggers if the constraint is ENFORCED. NOT
	 * ENFORCED constraints do not have these triggers.
	 */
	if (parentConstrIsEnforced)
	{
		Oid			insertTriggerOid,
					updateTriggerOid;

		GetForeignKeyCheckTriggers(trigrel,
								   partConstrOid, partConstrFrelid, partConstrRelid,
								   &insertTriggerOid, &updateTriggerOid);
		Assert(OidIsValid(insertTriggerOid) && OidIsValid(parentInsTrigger));
		TriggerSetParentTrigger(trigrel, insertTriggerOid, parentInsTrigger,
								RelationGetRelid(partition));
		Assert(OidIsValid(updateTriggerOid) && OidIsValid(parentUpdTrigger));
		TriggerSetParentTrigger(trigrel, updateTriggerOid, parentUpdTrigger,
								RelationGetRelid(partition));
	}

	/*
	 * We updated this pg_constraint row above to set its parent; validating
	 * it will cause its convalidated flag to change, so we need CCI here.  In
	 * addition, we need it unconditionally for the rare case where the parent
	 * table has *two* identical constraints; when reaching this function for
	 * the second one, we must have made our changes visible, otherwise we
	 * would try to attach both to this one.
	 */
	CommandCounterIncrement();

	/* If validation is needed, put it in the queue now. */
	if (queueValidation)
	{
		Relation	conrel;
		Oid			confrelid;

		conrel = table_open(ConstraintRelationId, RowExclusiveLock);

		partcontup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(partConstrOid));
		if (!HeapTupleIsValid(partcontup))
			elog(ERROR, "cache lookup failed for constraint %u", partConstrOid);

		confrelid = ((Form_pg_constraint) GETSTRUCT(partcontup))->confrelid;

		/* Use the same lock as for AT_ValidateConstraint */
		QueueFKConstraintValidation(wqueue, conrel, partition, confrelid,
									partcontup, ShareUpdateExclusiveLock);
		ReleaseSysCache(partcontup);
		table_close(conrel, RowExclusiveLock);
	}
}

/*
 * RemoveInheritedConstraint
 *
 * Removes the constraint and its associated trigger from the specified
 * relation, which inherited the given constraint.
 */
static void
RemoveInheritedConstraint(Relation conrel, Relation trigrel, Oid conoid,
						  Oid conrelid)
{
	ObjectAddresses *objs;
	HeapTuple	consttup;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	trigtup;

	ScanKeyInit(&key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(conrelid));

	scan = systable_beginscan(conrel,
							  ConstraintRelidTypidNameIndexId,
							  true, NULL, 1, &key);
	objs = new_object_addresses();
	while ((consttup = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint conform = (Form_pg_constraint) GETSTRUCT(consttup);

		if (conform->conparentid != conoid)
			continue;
		else
		{
			ObjectAddress addr;
			SysScanDesc scan2;
			ScanKeyData key2;
			int			n PG_USED_FOR_ASSERTS_ONLY;

			ObjectAddressSet(addr, ConstraintRelationId, conform->oid);
			add_exact_object_address(&addr, objs);

			/*
			 * First we must delete the dependency record that binds the
			 * constraint records together.
			 */
			n = deleteDependencyRecordsForSpecific(ConstraintRelationId,
												   conform->oid,
												   DEPENDENCY_INTERNAL,
												   ConstraintRelationId,
												   conoid);
			Assert(n == 1);		/* actually only one is expected */

			/*
			 * Now search for the triggers for this constraint and set them up
			 * for deletion too
			 */
			ScanKeyInit(&key2,
						Anum_pg_trigger_tgconstraint,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(conform->oid));
			scan2 = systable_beginscan(trigrel, TriggerConstraintIndexId,
									   true, NULL, 1, &key2);
			while ((trigtup = systable_getnext(scan2)) != NULL)
			{
				ObjectAddressSet(addr, TriggerRelationId,
								 ((Form_pg_trigger) GETSTRUCT(trigtup))->oid);
				add_exact_object_address(&addr, objs);
			}
			systable_endscan(scan2);
		}
	}
	/* make the dependency deletions visible */
	CommandCounterIncrement();
	performMultipleDeletions(objs, DROP_RESTRICT,
							 PERFORM_DELETION_INTERNAL);
	systable_endscan(scan);
}

/*
 * GetForeignKeyActionTriggers
 * 		Returns delete and update "action" triggers of the given relation
 * 		belonging to the given constraint
 */
static void
GetForeignKeyActionTriggers(Relation trigrel,
							Oid conoid, Oid confrelid, Oid conrelid,
							Oid *deleteTriggerOid,
							Oid *updateTriggerOid)
{
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	trigtup;

	*deleteTriggerOid = *updateTriggerOid = InvalidOid;
	ScanKeyInit(&key,
				Anum_pg_trigger_tgconstraint,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(conoid));

	scan = systable_beginscan(trigrel, TriggerConstraintIndexId, true,
							  NULL, 1, &key);
	while ((trigtup = systable_getnext(scan)) != NULL)
	{
		Form_pg_trigger trgform = (Form_pg_trigger) GETSTRUCT(trigtup);

		if (trgform->tgconstrrelid != conrelid)
			continue;
		if (trgform->tgrelid != confrelid)
			continue;
		/* Only ever look at "action" triggers on the PK side. */
		if (RI_FKey_trigger_type(trgform->tgfoid) != RI_TRIGGER_PK)
			continue;
		if (TRIGGER_FOR_DELETE(trgform->tgtype))
		{
			Assert(*deleteTriggerOid == InvalidOid);
			*deleteTriggerOid = trgform->oid;
		}
		else if (TRIGGER_FOR_UPDATE(trgform->tgtype))
		{
			Assert(*updateTriggerOid == InvalidOid);
			*updateTriggerOid = trgform->oid;
		}
#ifndef USE_ASSERT_CHECKING
		/* In an assert-enabled build, continue looking to find duplicates */
		if (OidIsValid(*deleteTriggerOid) && OidIsValid(*updateTriggerOid))
			break;
#endif
	}

	if (!OidIsValid(*deleteTriggerOid))
		elog(ERROR, "could not find ON DELETE action trigger of foreign key constraint %u",
			 conoid);
	if (!OidIsValid(*updateTriggerOid))
		elog(ERROR, "could not find ON UPDATE action trigger of foreign key constraint %u",
			 conoid);

	systable_endscan(scan);
}

/*
 * GetForeignKeyCheckTriggers
 * 		Returns insert and update "check" triggers of the given relation
 * 		belonging to the given constraint
 */
static void
GetForeignKeyCheckTriggers(Relation trigrel,
						   Oid conoid, Oid confrelid, Oid conrelid,
						   Oid *insertTriggerOid,
						   Oid *updateTriggerOid)
{
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	trigtup;

	*insertTriggerOid = *updateTriggerOid = InvalidOid;
	ScanKeyInit(&key,
				Anum_pg_trigger_tgconstraint,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(conoid));

	scan = systable_beginscan(trigrel, TriggerConstraintIndexId, true,
							  NULL, 1, &key);
	while ((trigtup = systable_getnext(scan)) != NULL)
	{
		Form_pg_trigger trgform = (Form_pg_trigger) GETSTRUCT(trigtup);

		if (trgform->tgconstrrelid != confrelid)
			continue;
		if (trgform->tgrelid != conrelid)
			continue;
		/* Only ever look at "check" triggers on the FK side. */
		if (RI_FKey_trigger_type(trgform->tgfoid) != RI_TRIGGER_FK)
			continue;
		if (TRIGGER_FOR_INSERT(trgform->tgtype))
		{
			Assert(*insertTriggerOid == InvalidOid);
			*insertTriggerOid = trgform->oid;
		}
		else if (TRIGGER_FOR_UPDATE(trgform->tgtype))
		{
			Assert(*updateTriggerOid == InvalidOid);
			*updateTriggerOid = trgform->oid;
		}
#ifndef USE_ASSERT_CHECKING
		/* In an assert-enabled build, continue looking to find duplicates. */
		if (OidIsValid(*insertTriggerOid) && OidIsValid(*updateTriggerOid))
			break;
#endif
	}

	if (!OidIsValid(*insertTriggerOid))
		elog(ERROR, "could not find ON INSERT check triggers of foreign key constraint %u",
			 conoid);
	if (!OidIsValid(*updateTriggerOid))
		elog(ERROR, "could not find ON UPDATE check triggers of foreign key constraint %u",
			 conoid);

	systable_endscan(scan);
}

/*
 * MarkInheritDetached
 *
 * Set inhdetachpending for a partition, for ATExecDetachPartition
 * in concurrent mode.  While at it, verify that no other partition is
 * already pending detach.
 */
static void
MarkInheritDetached(Relation child_rel, Relation parent_rel)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	inheritsTuple;
	bool		found = false;

	Assert(parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	/*
	 * Find pg_inherits entries by inhparent.  (We need to scan them all in
	 * order to verify that no other partition is pending detach.)
	 */
	catalogRelation = table_open(InheritsRelationId, RowExclusiveLock);
	ScanKeyInit(&key,
				Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(parent_rel)));
	scan = systable_beginscan(catalogRelation, InheritsParentIndexId,
							  true, NULL, 1, &key);

	while (HeapTupleIsValid(inheritsTuple = systable_getnext(scan)))
	{
		Form_pg_inherits inhForm;

		inhForm = (Form_pg_inherits) GETSTRUCT(inheritsTuple);
		if (inhForm->inhdetachpending)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("partition \"%s\" already pending detach in partitioned table \"%s.%s\"",
						   get_rel_name(inhForm->inhrelid),
						   get_namespace_name(parent_rel->rd_rel->relnamespace),
						   RelationGetRelationName(parent_rel)),
					errhint("Use ALTER TABLE ... DETACH PARTITION ... FINALIZE to complete the pending detach operation."));

		if (inhForm->inhrelid == RelationGetRelid(child_rel))
		{
			HeapTuple	newtup;

			newtup = heap_copytuple(inheritsTuple);
			((Form_pg_inherits) GETSTRUCT(newtup))->inhdetachpending = true;

			CatalogTupleUpdate(catalogRelation,
							   &inheritsTuple->t_self,
							   newtup);
			found = true;
			heap_freetuple(newtup);
			/* keep looking, to ensure we catch others pending detach */
		}
	}

	/* Done */
	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation \"%s\" is not a partition of relation \"%s\"",
						RelationGetRelationName(child_rel),
						RelationGetRelationName(parent_rel))));
}

/*
 * Transform any expressions present in the partition key
 *
 * Returns a transformed PartitionSpec.
 */
PartitionSpec *
transformPartitionSpec(Relation rel, PartitionSpec *partspec)
{
	PartitionSpec *newspec;
	ParseState *pstate;
	ParseNamespaceItem *nsitem;
	ListCell   *l;

	newspec = makeNode(PartitionSpec);

	newspec->strategy = partspec->strategy;
	newspec->partParams = NIL;
	newspec->location = partspec->location;

	/* Check valid number of columns for strategy */
	if (partspec->strategy == PARTITION_STRATEGY_LIST &&
		list_length(partspec->partParams) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("cannot use \"list\" partition strategy with more than one column")));

	/*
	 * Create a dummy ParseState and insert the target relation as its sole
	 * rangetable entry.  We need a ParseState for transformExpr.
	 */
	pstate = make_parsestate(NULL);
	nsitem = addRangeTableEntryForRelation(pstate, rel, AccessShareLock,
										   NULL, false, true);
	addNSItemToQuery(pstate, nsitem, true, true, true);

	/* take care of any partition expressions */
	foreach(l, partspec->partParams)
	{
		PartitionElem *pelem = lfirst_node(PartitionElem, l);

		if (pelem->expr)
		{
			/* Copy, to avoid scribbling on the input */
			pelem = copyObject(pelem);

			/* Now do parse transformation of the expression */
			pelem->expr = transformExpr(pstate, pelem->expr,
										EXPR_KIND_PARTITION_EXPRESSION);

			/* we have to fix its collations too */
			assign_expr_collations(pstate, pelem->expr);
		}

		newspec->partParams = lappend(newspec->partParams, pelem);
	}

	return newspec;
}

/*
 * Compute per-partition-column information from a list of PartitionElems.
 * Expressions in the PartitionElems must be parse-analyzed already.
 */
void
ComputePartitionAttrs(ParseState *pstate, Relation rel, List *partParams, AttrNumber *partattrs,
					  List **partexprs, Oid *partopclass, Oid *partcollation,
					  PartitionStrategy strategy)
{
	int			attn;
	ListCell   *lc;
	Oid			am_oid;

	attn = 0;
	foreach(lc, partParams)
	{
		PartitionElem *pelem = lfirst_node(PartitionElem, lc);
		Oid			atttype;
		Oid			attcollation;

		if (pelem->name != NULL)
		{
			/* Simple attribute reference */
			HeapTuple	atttuple;
			Form_pg_attribute attform;

			atttuple = SearchSysCacheAttName(RelationGetRelid(rel),
											 pelem->name);
			if (!HeapTupleIsValid(atttuple))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" named in partition key does not exist",
								pelem->name),
						 parser_errposition(pstate, pelem->location)));
			attform = (Form_pg_attribute) GETSTRUCT(atttuple);

			if (attform->attnum <= 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot use system column \"%s\" in partition key",
								pelem->name),
						 parser_errposition(pstate, pelem->location)));

			/*
			 * Stored generated columns cannot work: They are computed after
			 * BEFORE triggers, but partition routing is done before all
			 * triggers.  Maybe virtual generated columns could be made to
			 * work, but then they would need to be handled as an expression
			 * below.
			 */
			if (attform->attgenerated)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot use generated column in partition key"),
						 errdetail("Column \"%s\" is a generated column.",
								   pelem->name),
						 parser_errposition(pstate, pelem->location)));

			partattrs[attn] = attform->attnum;
			atttype = attform->atttypid;
			attcollation = attform->attcollation;
			ReleaseSysCache(atttuple);
		}
		else
		{
			/* Expression */
			Node	   *expr = pelem->expr;
			char		partattname[16];
			Bitmapset  *expr_attrs = NULL;
			int			i;

			Assert(expr != NULL);
			atttype = exprType(expr);
			attcollation = exprCollation(expr);

			/*
			 * The expression must be of a storable type (e.g., not RECORD).
			 * The test is the same as for whether a table column is of a safe
			 * type (which is why we needn't check for the non-expression
			 * case).
			 */
			snprintf(partattname, sizeof(partattname), "%d", attn + 1);
			CheckAttributeType(partattname,
							   atttype, attcollation,
							   NIL, CHKATYPE_IS_PARTKEY);

			/*
			 * Strip any top-level COLLATE clause.  This ensures that we treat
			 * "x COLLATE y" and "(x COLLATE y)" alike.
			 */
			while (IsA(expr, CollateExpr))
				expr = (Node *) ((CollateExpr *) expr)->arg;

			/*
			 * Examine all the columns in the partition key expression. When
			 * the whole-row reference is present, examine all the columns of
			 * the partitioned table.
			 */
			pull_varattnos(expr, 1, &expr_attrs);
			if (bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, expr_attrs))
			{
				expr_attrs = bms_add_range(expr_attrs,
										   1 - FirstLowInvalidHeapAttributeNumber,
										   RelationGetNumberOfAttributes(rel) - FirstLowInvalidHeapAttributeNumber);
				expr_attrs = bms_del_member(expr_attrs, 0 - FirstLowInvalidHeapAttributeNumber);
			}

			i = -1;
			while ((i = bms_next_member(expr_attrs, i)) >= 0)
			{
				AttrNumber	attno = i + FirstLowInvalidHeapAttributeNumber;

				Assert(attno != 0);

				/*
				 * Cannot allow system column references, since that would
				 * make partition routing impossible: their values won't be
				 * known yet when we need to do that.
				 */
				if (attno < 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("partition key expressions cannot contain system column references")));

				/*
				 * Stored generated columns cannot work: They are computed
				 * after BEFORE triggers, but partition routing is done before
				 * all triggers.  Virtual generated columns could probably
				 * work, but it would require more work elsewhere (for example
				 * SET EXPRESSION would need to check whether the column is
				 * used in partition keys).  Seems safer to prohibit for now.
				 */
				if (TupleDescAttr(RelationGetDescr(rel), attno - 1)->attgenerated)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("cannot use generated column in partition key"),
							 errdetail("Column \"%s\" is a generated column.",
									   get_attname(RelationGetRelid(rel), attno, false)),
							 parser_errposition(pstate, pelem->location)));
			}

			if (IsA(expr, Var) &&
				((Var *) expr)->varattno > 0)
			{

				/*
				 * User wrote "(column)" or "(column COLLATE something)".
				 * Treat it like simple attribute anyway.
				 */
				partattrs[attn] = ((Var *) expr)->varattno;
			}
			else
			{
				partattrs[attn] = 0;	/* marks the column as expression */
				*partexprs = lappend(*partexprs, expr);

				/*
				 * transformPartitionSpec() should have already rejected
				 * subqueries, aggregates, window functions, and SRFs, based
				 * on the EXPR_KIND_ for partition expressions.
				 */

				/*
				 * Preprocess the expression before checking for mutability.
				 * This is essential for the reasons described in
				 * contain_mutable_functions_after_planning.  However, we call
				 * expression_planner for ourselves rather than using that
				 * function, because if constant-folding reduces the
				 * expression to a constant, we'd like to know that so we can
				 * complain below.
				 *
				 * Like contain_mutable_functions_after_planning, assume that
				 * expression_planner won't scribble on its input, so this
				 * won't affect the partexprs entry we saved above.
				 */
				expr = (Node *) expression_planner((Expr *) expr);

				/*
				 * Partition expressions cannot contain mutable functions,
				 * because a given row must always map to the same partition
				 * as long as there is no change in the partition boundary
				 * structure.
				 */
				if (contain_mutable_functions(expr))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("functions in partition key expression must be marked IMMUTABLE")));

				/*
				 * While it is not exactly *wrong* for a partition expression
				 * to be a constant, it seems better to reject such keys.
				 */
				if (IsA(expr, Const))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("cannot use constant expression as partition key")));
			}
		}

		/*
		 * Apply collation override if any
		 */
		if (pelem->collation)
			attcollation = get_collation_oid(pelem->collation, false);

		/*
		 * Check we have a collation iff it's a collatable type.  The only
		 * expected failures here are (1) COLLATE applied to a noncollatable
		 * type, or (2) partition expression had an unresolved collation. But
		 * we might as well code this to be a complete consistency check.
		 */
		if (type_is_collatable(atttype))
		{
			if (!OidIsValid(attcollation))
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_COLLATION),
						 errmsg("could not determine which collation to use for partition expression"),
						 errhint("Use the COLLATE clause to set the collation explicitly.")));
		}
		else
		{
			if (OidIsValid(attcollation))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("collations are not supported by type %s",
								format_type_be(atttype))));
		}

		partcollation[attn] = attcollation;

		/*
		 * Identify the appropriate operator class.  For list and range
		 * partitioning, we use a btree operator class; hash partitioning uses
		 * a hash operator class.
		 */
		if (strategy == PARTITION_STRATEGY_HASH)
			am_oid = HASH_AM_OID;
		else
			am_oid = BTREE_AM_OID;

		if (!pelem->opclass)
		{
			partopclass[attn] = GetDefaultOpClass(atttype, am_oid);

			if (!OidIsValid(partopclass[attn]))
			{
				if (strategy == PARTITION_STRATEGY_HASH)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("data type %s has no default operator class for access method \"%s\"",
									format_type_be(atttype), "hash"),
							 errhint("You must specify a hash operator class or define a default hash operator class for the data type.")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("data type %s has no default operator class for access method \"%s\"",
									format_type_be(atttype), "btree"),
							 errhint("You must specify a btree operator class or define a default btree operator class for the data type.")));
			}
		}
		else
			partopclass[attn] = ResolveOpClass(pelem->opclass,
											   atttype,
											   am_oid == HASH_AM_OID ? "hash" : "btree",
											   am_oid);

		attn++;
	}
}

/*
 * PartConstraintImpliedByRelConstraint
 *		Do scanrel's existing constraints imply the partition constraint?
 *
 * "Existing constraints" include its check constraints and column-level
 * not-null constraints.  partConstraint describes the partition constraint,
 * in implicit-AND form.
 */
bool
PartConstraintImpliedByRelConstraint(Relation scanrel,
									 List *partConstraint)
{
	List	   *existConstraint = NIL;
	TupleConstr *constr = RelationGetDescr(scanrel)->constr;
	int			i;

	if (constr && constr->has_not_null)
	{
		int			natts = scanrel->rd_att->natts;

		for (i = 1; i <= natts; i++)
		{
			CompactAttribute *att = TupleDescCompactAttr(scanrel->rd_att, i - 1);

			/* invalid not-null constraint must be ignored here */
			if (att->attnullability == ATTNULLABLE_VALID && !att->attisdropped)
			{
				Form_pg_attribute wholeatt = TupleDescAttr(scanrel->rd_att, i - 1);
				NullTest   *ntest = makeNode(NullTest);

				ntest->arg = (Expr *) makeVar(1,
											  i,
											  wholeatt->atttypid,
											  wholeatt->atttypmod,
											  wholeatt->attcollation,
											  0);
				ntest->nulltesttype = IS_NOT_NULL;

				/*
				 * argisrow=false is correct even for a composite column,
				 * because attnotnull does not represent a SQL-spec IS NOT
				 * NULL test in such a case, just IS DISTINCT FROM NULL.
				 */
				ntest->argisrow = false;
				ntest->location = -1;
				existConstraint = lappend(existConstraint, ntest);
			}
		}
	}

	return ConstraintImpliedByRelConstraint(scanrel, partConstraint, existConstraint);
}

/*
 * ConstraintImpliedByRelConstraint
 *		Do scanrel's existing constraints imply the given constraint?
 *
 * testConstraint is the constraint to validate. provenConstraint is a
 * caller-provided list of conditions which this function may assume
 * to be true. Both provenConstraint and testConstraint must be in
 * implicit-AND form, must only contain immutable clauses, and must
 * contain only Vars with varno = 1.
 */
bool
ConstraintImpliedByRelConstraint(Relation scanrel, List *testConstraint, List *provenConstraint)
{
	List	   *existConstraint = list_copy(provenConstraint);
	TupleConstr *constr = RelationGetDescr(scanrel)->constr;
	int			num_check,
				i;

	num_check = (constr != NULL) ? constr->num_check : 0;
	for (i = 0; i < num_check; i++)
	{
		Node	   *cexpr;

		/*
		 * If this constraint hasn't been fully validated yet, we must ignore
		 * it here.
		 */
		if (!constr->check[i].ccvalid)
			continue;

		/*
		 * NOT ENFORCED constraints are always marked as invalid, which should
		 * have been ignored.
		 */
		Assert(constr->check[i].ccenforced);

		cexpr = stringToNode(constr->check[i].ccbin);

		/*
		 * Run each expression through const-simplification and
		 * canonicalization.  It is necessary, because we will be comparing it
		 * to similarly-processed partition constraint expressions, and may
		 * fail to detect valid matches without this.
		 */
		cexpr = eval_const_expressions(NULL, cexpr);
		cexpr = (Node *) canonicalize_qual((Expr *) cexpr, true);

		existConstraint = list_concat(existConstraint,
									  make_ands_implicit((Expr *) cexpr));
	}

	/*
	 * Try to make the proof.  Since we are comparing CHECK constraints, we
	 * need to use weak implication, i.e., we assume existConstraint is
	 * not-false and try to prove the same for testConstraint.
	 *
	 * Note that predicate_implied_by assumes its first argument is known
	 * immutable.  That should always be true for both NOT NULL and partition
	 * constraints, so we don't test it here.
	 */
	return predicate_implied_by(testConstraint, existConstraint, true);
}

/*
 * QueuePartitionConstraintValidation
 *
 * Add an entry to wqueue to have the given partition constraint validated by
 * Phase 3, for the given relation, and all its children.
 *
 * We first verify whether the given constraint is implied by pre-existing
 * relation constraints; if it is, there's no need to scan the table to
 * validate, so don't queue in that case.
 */
static void
QueuePartitionConstraintValidation(List **wqueue, Relation scanrel,
								   List *partConstraint,
								   bool validate_default)
{
	/*
	 * Based on the table's existing constraints, determine whether or not we
	 * may skip scanning the table.
	 */
	if (PartConstraintImpliedByRelConstraint(scanrel, partConstraint))
	{
		if (!validate_default)
			ereport(DEBUG1,
					(errmsg_internal("partition constraint for table \"%s\" is implied by existing constraints",
									 RelationGetRelationName(scanrel))));
		else
			ereport(DEBUG1,
					(errmsg_internal("updated partition constraint for default partition \"%s\" is implied by existing constraints",
									 RelationGetRelationName(scanrel))));
		return;
	}

	/*
	 * Constraints proved insufficient. For plain relations, queue a
	 * validation item now; for partitioned tables, recurse to process each
	 * partition.
	 */
	if (scanrel->rd_rel->relkind == RELKIND_RELATION)
	{
		AlteredTableInfo *tab;

		/* Grab a work queue entry. */
		tab = ATGetQueueEntry(wqueue, scanrel);
		Assert(tab->partition_constraint == NULL);
		tab->partition_constraint = (Expr *) linitial(partConstraint);
		tab->validate_default = validate_default;
	}
	else if (scanrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		PartitionDesc partdesc = RelationGetPartitionDesc(scanrel, true);
		int			i;

		for (i = 0; i < partdesc->nparts; i++)
		{
			Relation	part_rel;
			List	   *thisPartConstraint;

			/*
			 * This is the minimum lock we need to prevent deadlocks.
			 */
			part_rel = table_open(partdesc->oids[i], AccessExclusiveLock);

			/*
			 * Adjust the constraint for scanrel so that it matches this
			 * partition's attribute numbers.
			 */
			thisPartConstraint =
				map_partition_varattnos(partConstraint, 1,
										part_rel, scanrel);

			QueuePartitionConstraintValidation(wqueue, part_rel,
											   thisPartConstraint,
											   validate_default);
			table_close(part_rel, NoLock);	/* keep lock till commit */
		}
	}
}

/*
 * attachPartitionTable: attach a new partition to the partitioned table
 *
 * wqueue: the ALTER TABLE work queue; can be NULL when not running as part
 *   of an ALTER TABLE sequence.
 * rel: partitioned relation;
 * attachrel: relation of attached partition;
 * bound: bounds of attached relation.
 */
static void
attachPartitionTable(List **wqueue, Relation rel, Relation attachrel, PartitionBoundSpec *bound)
{
	/*
	 * Create an inheritance; the relevant checks are performed inside the
	 * function.
	 */
	CreateInheritance(attachrel, rel, true);

	/* Update the pg_class entry. */
	StorePartitionBound(attachrel, rel, bound);

	/* Ensure there exists a correct set of indexes in the partition. */
	AttachPartitionEnsureIndexes(wqueue, rel, attachrel);

	/* and triggers */
	CloneRowTriggersToPartition(rel, attachrel);

	/*
	 * Clone foreign key constraints.  Callee is responsible for setting up
	 * for phase 3 constraint verification.
	 */
	CloneForeignKeyConstraints(wqueue, rel, attachrel);
}

/*
 * ALTER TABLE <name> ATTACH PARTITION <partition-name> FOR VALUES
 *
 * Return the address of the newly attached partition.
 */
ObjectAddress
ATExecAttachPartition(List **wqueue, Relation rel, PartitionCmd *cmd,
					  AlterTableUtilityContext *context)
{
	Relation	attachrel,
				catalog;
	List	   *attachrel_children;
	List	   *partConstraint;
	SysScanDesc scan;
	ScanKeyData skey;
	AttrNumber	attno;
	int			natts;
	TupleDesc	tupleDesc;
	ObjectAddress address;
	const char *trigger_name;
	Oid			defaultPartOid;
	List	   *partBoundConstraint;
	ParseState *pstate = make_parsestate(NULL);

	pstate->p_sourcetext = context->queryString;

	/*
	 * We must lock the default partition if one exists, because attaching a
	 * new partition will change its partition constraint.
	 */
	defaultPartOid =
		get_default_oid_from_partdesc(RelationGetPartitionDesc(rel, true));
	if (OidIsValid(defaultPartOid))
		LockRelationOid(defaultPartOid, AccessExclusiveLock);

	attachrel = table_openrv(cmd->name, AccessExclusiveLock);

	/*
	 * XXX I think it'd be a good idea to grab locks on all tables referenced
	 * by FKs at this point also.
	 */

	/*
	 * Must be owner of both parent and source table -- parent was checked by
	 * ATSimplePermissions call in ATPrepCmd
	 */
	ATSimplePermissions(AT_AttachPartition, attachrel,
						ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);

	/* A partition can only have one parent */
	if (attachrel->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is already a partition",
						RelationGetRelationName(attachrel))));

	if (OidIsValid(attachrel->rd_rel->reloftype))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach a typed table as partition")));

	/*
	 * Table being attached should not already be part of inheritance; either
	 * as a child table...
	 */
	catalog = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&skey,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(attachrel)));
	scan = systable_beginscan(catalog, InheritsRelidSeqnoIndexId, true,
							  NULL, 1, &skey);
	if (HeapTupleIsValid(systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach inheritance child as partition")));
	systable_endscan(scan);

	/* ...or as a parent table (except the case when it is partitioned) */
	ScanKeyInit(&skey,
				Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(attachrel)));
	scan = systable_beginscan(catalog, InheritsParentIndexId, true, NULL,
							  1, &skey);
	if (HeapTupleIsValid(systable_getnext(scan)) &&
		attachrel->rd_rel->relkind == RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach inheritance parent as partition")));
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);

	/*
	 * Prevent circularity by seeing if rel is a partition of attachrel. (In
	 * particular, this disallows making a rel a partition of itself.)
	 *
	 * We do that by checking if rel is a member of the list of attachrel's
	 * partitions provided the latter is partitioned at all.  We want to avoid
	 * having to construct this list again, so we request the strongest lock
	 * on all partitions.  We need the strongest lock, because we may decide
	 * to scan them if we find out that the table being attached (or its leaf
	 * partitions) may contain rows that violate the partition constraint. If
	 * the table has a constraint that would prevent such rows, which by
	 * definition is present in all the partitions, we need not scan the
	 * table, nor its partitions.  But we cannot risk a deadlock by taking a
	 * weaker lock now and the stronger one only when needed.
	 */
	attachrel_children = find_all_inheritors(RelationGetRelid(attachrel),
											 AccessExclusiveLock, NULL);
	if (list_member_oid(attachrel_children, RelationGetRelid(rel)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("circular inheritance not allowed"),
				 errdetail("\"%s\" is already a child of \"%s\".",
						   RelationGetRelationName(rel),
						   RelationGetRelationName(attachrel))));

	/* If the parent is permanent, so must be all of its partitions. */
	if (rel->rd_rel->relpersistence != RELPERSISTENCE_TEMP &&
		attachrel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach a temporary relation as partition of permanent relation \"%s\"",
						RelationGetRelationName(rel))));

	/* Temp parent cannot have a partition that is itself not a temp */
	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		attachrel->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach a permanent relation as partition of temporary relation \"%s\"",
						RelationGetRelationName(rel))));

	/* If the parent is temp, it must belong to this session */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach as partition of temporary relation of another session")));

	/* Ditto for the partition */
	if (RELATION_IS_OTHER_TEMP(attachrel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach temporary relation of another session as partition")));

	/*
	 * Check if attachrel has any identity columns or any columns that aren't
	 * in the parent.
	 */
	tupleDesc = RelationGetDescr(attachrel);
	natts = tupleDesc->natts;
	for (attno = 1; attno <= natts; attno++)
	{
		Form_pg_attribute attribute = TupleDescAttr(tupleDesc, attno - 1);
		char	   *attributeName = NameStr(attribute->attname);

		/* Ignore dropped */
		if (attribute->attisdropped)
			continue;

		if (attribute->attidentity)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("table \"%s\" being attached contains an identity column \"%s\"",
						   RelationGetRelationName(attachrel), attributeName),
					errdetail("The new partition may not contain an identity column."));

		/* Try to find the column in parent (matching on column name) */
		if (!SearchSysCacheExists2(ATTNAME,
								   ObjectIdGetDatum(RelationGetRelid(rel)),
								   CStringGetDatum(attributeName)))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table \"%s\" contains column \"%s\" not found in parent \"%s\"",
							RelationGetRelationName(attachrel), attributeName,
							RelationGetRelationName(rel)),
					 errdetail("The new partition may contain only the columns present in parent.")));
	}

	/*
	 * If child_rel has row-level triggers with transition tables, we
	 * currently don't allow it to become a partition.  See also prohibitions
	 * in ATExecAddInherit() and CreateTrigger().
	 */
	trigger_name = FindTriggerIncompatibleWithInheritance(attachrel->trigdesc);
	if (trigger_name != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("trigger \"%s\" prevents table \"%s\" from becoming a partition",
						trigger_name, RelationGetRelationName(attachrel)),
				 errdetail("ROW triggers with transition tables are not supported on partitions.")));

	/*
	 * Check that the new partition's bound is valid and does not overlap any
	 * of existing partitions of the parent - note that it does not return on
	 * error.
	 */
	check_new_partition_bound(RelationGetRelationName(attachrel), rel,
							  cmd->bound, pstate);

	attachPartitionTable(wqueue, rel, attachrel, cmd->bound);

	/*
	 * Generate a partition constraint from the partition bound specification.
	 * If the parent itself is a partition, make sure to include its
	 * constraint as well.
	 */
	partBoundConstraint = get_qual_from_partbound(rel, cmd->bound);

	/*
	 * Use list_concat_copy() to avoid modifying partBoundConstraint in place,
	 * since it's needed later to construct the constraint expression for
	 * validating against the default partition, if any.
	 */
	partConstraint = list_concat_copy(partBoundConstraint,
									  RelationGetPartitionQual(rel));

	/* Skip validation if there are no constraints to validate. */
	if (partConstraint)
	{
		/*
		 * Run the partition quals through const-simplification similar to
		 * check constraints.  We skip canonicalize_qual, though, because
		 * partition quals should be in canonical form already.
		 */
		partConstraint =
			(List *) eval_const_expressions(NULL,
											(Node *) partConstraint);

		/* XXX this sure looks wrong */
		partConstraint = list_make1(make_ands_explicit(partConstraint));

		/*
		 * Adjust the generated constraint to match this partition's attribute
		 * numbers.
		 */
		partConstraint = map_partition_varattnos(partConstraint, 1, attachrel,
												 rel);

		/* Validate partition constraints against the table being attached. */
		QueuePartitionConstraintValidation(wqueue, attachrel, partConstraint,
										   false);
	}

	/*
	 * If we're attaching a partition other than the default partition and a
	 * default one exists, then that partition's partition constraint changes,
	 * so add an entry to the work queue to validate it, too.  (We must not do
	 * this when the partition being attached is the default one; we already
	 * did it above!)
	 */
	if (OidIsValid(defaultPartOid))
	{
		Relation	defaultrel;
		List	   *defPartConstraint;

		Assert(!cmd->bound->is_default);

		/* we already hold a lock on the default partition */
		defaultrel = table_open(defaultPartOid, NoLock);
		defPartConstraint =
			get_proposed_default_constraint(partBoundConstraint);

		/*
		 * Map the Vars in the constraint expression from rel's attnos to
		 * defaultrel's.
		 */
		defPartConstraint =
			map_partition_varattnos(defPartConstraint,
									1, defaultrel, rel);
		QueuePartitionConstraintValidation(wqueue, defaultrel,
										   defPartConstraint, true);

		/* keep our lock until commit. */
		table_close(defaultrel, NoLock);
	}

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(attachrel));

	/*
	 * If the partition we just attached is partitioned itself, invalidate
	 * relcache for all descendent partitions too to ensure that their
	 * rd_partcheck expression trees are rebuilt; partitions already locked at
	 * the beginning of this function.
	 */
	if (attachrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		ListCell   *l;

		foreach(l, attachrel_children)
		{
			CacheInvalidateRelcacheByRelid(lfirst_oid(l));
		}
	}

	/* keep our lock until commit */
	table_close(attachrel, NoLock);

	return address;
}

/*
 * AttachPartitionEnsureIndexes
 *		subroutine for ATExecAttachPartition to create/match indexes
 *
 * Enforce the indexing rule for partitioned tables during ALTER TABLE / ATTACH
 * PARTITION: every partition must have an index attached to each index on the
 * partitioned table.
 */
static void
AttachPartitionEnsureIndexes(List **wqueue, Relation rel, Relation attachrel)
{
	List	   *idxes;
	List	   *attachRelIdxs;
	Relation   *attachrelIdxRels;
	IndexInfo **attachInfos;
	ListCell   *cell;
	MemoryContext cxt;
	MemoryContext oldcxt;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"AttachPartitionEnsureIndexes",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	idxes = RelationGetIndexList(rel);
	attachRelIdxs = RelationGetIndexList(attachrel);
	attachrelIdxRels = palloc_array(Relation, list_length(attachRelIdxs));
	attachInfos = palloc_array(IndexInfo *, list_length(attachRelIdxs));

	/* Build arrays of all existing indexes and their IndexInfos */
	foreach_oid(cldIdxId, attachRelIdxs)
	{
		int			i = foreach_current_index(cldIdxId);

		attachrelIdxRels[i] = index_open(cldIdxId, AccessShareLock);
		attachInfos[i] = BuildIndexInfo(attachrelIdxRels[i]);
	}

	/*
	 * If we're attaching a foreign table, we must fail if any of the indexes
	 * is a constraint index; otherwise, there's nothing to do here.  Do this
	 * before starting work, to avoid wasting the effort of building a few
	 * non-unique indexes before coming across a unique one.
	 */
	if (attachrel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		foreach(cell, idxes)
		{
			Oid			idx = lfirst_oid(cell);
			Relation	idxRel = index_open(idx, AccessShareLock);

			if (idxRel->rd_index->indisunique ||
				idxRel->rd_index->indisprimary)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot attach foreign table \"%s\" as partition of partitioned table \"%s\"",
								RelationGetRelationName(attachrel),
								RelationGetRelationName(rel)),
						 errdetail("Partitioned table \"%s\" contains unique indexes.",
								   RelationGetRelationName(rel))));
			index_close(idxRel, AccessShareLock);
		}

		goto out;
	}

	/*
	 * For each index on the partitioned table, find a matching one in the
	 * partition-to-be; if one is not found, create one.
	 */
	foreach(cell, idxes)
	{
		Oid			idx = lfirst_oid(cell);
		Relation	idxRel = index_open(idx, AccessShareLock);
		IndexInfo  *info;
		AttrMap    *attmap;
		bool		found = false;
		Oid			constraintOid;

		/*
		 * Ignore indexes in the partitioned table other than partitioned
		 * indexes.
		 */
		if (idxRel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
		{
			index_close(idxRel, AccessShareLock);
			continue;
		}

		/* construct an indexinfo to compare existing indexes against */
		info = BuildIndexInfo(idxRel);
		attmap = build_attrmap_by_name(RelationGetDescr(attachrel),
									   RelationGetDescr(rel),
									   false);
		constraintOid = get_relation_idx_constraint_oid(RelationGetRelid(rel), idx);

		/*
		 * Scan the list of existing indexes in the partition-to-be, and mark
		 * the first matching, valid, unattached one we find, if any, as
		 * partition of the parent index.  If we find one, we're done.
		 */
		for (int i = 0; i < list_length(attachRelIdxs); i++)
		{
			Oid			cldIdxId = RelationGetRelid(attachrelIdxRels[i]);
			Oid			cldConstrOid = InvalidOid;

			/* does this index have a parent?  if so, can't use it */
			if (attachrelIdxRels[i]->rd_rel->relispartition)
				continue;

			/* If this index is invalid, can't use it */
			if (!attachrelIdxRels[i]->rd_index->indisvalid)
				continue;

			if (CompareIndexInfo(attachInfos[i], info,
								 attachrelIdxRels[i]->rd_indcollation,
								 idxRel->rd_indcollation,
								 attachrelIdxRels[i]->rd_opfamily,
								 idxRel->rd_opfamily,
								 attmap))
			{
				/*
				 * If this index is being created in the parent because of a
				 * constraint, then the child needs to have a constraint also,
				 * so look for one.  If there is no such constraint, this
				 * index is no good, so keep looking.
				 */
				if (OidIsValid(constraintOid))
				{
					cldConstrOid =
						get_relation_idx_constraint_oid(RelationGetRelid(attachrel),
														cldIdxId);
					/* no dice */
					if (!OidIsValid(cldConstrOid))
						continue;

					/* Ensure they're both the same type of constraint */
					if (get_constraint_type(constraintOid) !=
						get_constraint_type(cldConstrOid))
						continue;
				}

				/* bingo. */
				IndexSetParentIndex(attachrelIdxRels[i], idx);
				if (OidIsValid(constraintOid))
					ConstraintSetParentConstraint(cldConstrOid, constraintOid,
												  RelationGetRelid(attachrel));
				found = true;

				CommandCounterIncrement();
				break;
			}
		}

		/*
		 * If no suitable index was found in the partition-to-be, create one
		 * now.  Note that if this is a PK, not-null constraints must already
		 * exist.
		 */
		if (!found)
		{
			IndexStmt  *stmt;
			Oid			conOid;

			stmt = generateClonedIndexStmt(NULL,
										   idxRel, attmap,
										   &conOid);
			DefineIndex(NULL,
						RelationGetRelid(attachrel), stmt, InvalidOid,
						RelationGetRelid(idxRel),
						conOid,
						-1,
						true, false, false, false, false);
		}

		index_close(idxRel, AccessShareLock);
	}

out:
	/* Clean up. */
	for (int i = 0; i < list_length(attachRelIdxs); i++)
		index_close(attachrelIdxRels[i], AccessShareLock);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}

/*
 * CloneRowTriggersToPartition
 *		subroutine for ATExecAttachPartition/DefineRelation to create row
 *		triggers on partitions
 */
void
CloneRowTriggersToPartition(Relation parent, Relation partition)
{
	Relation	pg_trigger;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	MemoryContext perTupCxt;

	ScanKeyInit(&key, Anum_pg_trigger_tgrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(parent)));
	pg_trigger = table_open(TriggerRelationId, RowExclusiveLock);
	scan = systable_beginscan(pg_trigger, TriggerRelidNameIndexId,
							  true, NULL, 1, &key);

	perTupCxt = AllocSetContextCreate(CurrentMemoryContext,
									  "clone trig", ALLOCSET_SMALL_SIZES);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_trigger trigForm = (Form_pg_trigger) GETSTRUCT(tuple);
		CreateTrigStmt *trigStmt;
		Node	   *qual = NULL;
		Datum		value;
		bool		isnull;
		List	   *cols = NIL;
		List	   *trigargs = NIL;
		MemoryContext oldcxt;

		/*
		 * Ignore statement-level triggers; those are not cloned.
		 */
		if (!TRIGGER_FOR_ROW(trigForm->tgtype))
			continue;

		/*
		 * Don't clone internal triggers, because the constraint cloning code
		 * will.
		 */
		if (trigForm->tgisinternal)
			continue;

		/*
		 * Complain if we find an unexpected trigger type.
		 */
		if (!TRIGGER_FOR_BEFORE(trigForm->tgtype) &&
			!TRIGGER_FOR_AFTER(trigForm->tgtype))
			elog(ERROR, "unexpected trigger \"%s\" found",
				 NameStr(trigForm->tgname));

		/* Use short-lived context for CREATE TRIGGER */
		oldcxt = MemoryContextSwitchTo(perTupCxt);

		/*
		 * If there is a WHEN clause, generate a 'cooked' version of it that's
		 * appropriate for the partition.
		 */
		value = heap_getattr(tuple, Anum_pg_trigger_tgqual,
							 RelationGetDescr(pg_trigger), &isnull);
		if (!isnull)
		{
			qual = stringToNode(TextDatumGetCString(value));
			qual = (Node *) map_partition_varattnos((List *) qual, PRS2_OLD_VARNO,
													partition, parent);
			qual = (Node *) map_partition_varattnos((List *) qual, PRS2_NEW_VARNO,
													partition, parent);
		}

		/*
		 * If there is a column list, transform it to a list of column names.
		 * Note we don't need to map this list in any way ...
		 */
		if (trigForm->tgattr.dim1 > 0)
		{
			int			i;

			for (i = 0; i < trigForm->tgattr.dim1; i++)
			{
				Form_pg_attribute col;

				col = TupleDescAttr(parent->rd_att,
									trigForm->tgattr.values[i] - 1);
				cols = lappend(cols,
							   makeString(pstrdup(NameStr(col->attname))));
			}
		}

		/* Reconstruct trigger arguments list. */
		if (trigForm->tgnargs > 0)
		{
			char	   *p;

			value = heap_getattr(tuple, Anum_pg_trigger_tgargs,
								 RelationGetDescr(pg_trigger), &isnull);
			if (isnull)
				elog(ERROR, "tgargs is null for trigger \"%s\" in partition \"%s\"",
					 NameStr(trigForm->tgname), RelationGetRelationName(partition));

			p = (char *) VARDATA_ANY(DatumGetByteaPP(value));

			for (int i = 0; i < trigForm->tgnargs; i++)
			{
				trigargs = lappend(trigargs, makeString(pstrdup(p)));
				p += strlen(p) + 1;
			}
		}

		trigStmt = makeNode(CreateTrigStmt);
		trigStmt->replace = false;
		trigStmt->isconstraint = OidIsValid(trigForm->tgconstraint);
		trigStmt->trigname = NameStr(trigForm->tgname);
		trigStmt->relation = NULL;
		trigStmt->funcname = NULL;	/* passed separately */
		trigStmt->args = trigargs;
		trigStmt->row = true;
		trigStmt->timing = trigForm->tgtype & TRIGGER_TYPE_TIMING_MASK;
		trigStmt->events = trigForm->tgtype & TRIGGER_TYPE_EVENT_MASK;
		trigStmt->columns = cols;
		trigStmt->whenClause = NULL;	/* passed separately */
		trigStmt->transitionRels = NIL; /* not supported at present */
		trigStmt->deferrable = trigForm->tgdeferrable;
		trigStmt->initdeferred = trigForm->tginitdeferred;
		trigStmt->constrrel = NULL; /* passed separately */

		CreateTriggerFiringOn(trigStmt, NULL, RelationGetRelid(partition),
							  trigForm->tgconstrrelid, InvalidOid, InvalidOid,
							  trigForm->tgfoid, trigForm->oid, qual,
							  false, true, trigForm->tgenabled);

		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(perTupCxt);
	}

	MemoryContextDelete(perTupCxt);

	systable_endscan(scan);
	table_close(pg_trigger, RowExclusiveLock);
}

/*
 * ALTER TABLE DETACH PARTITION
 *
 * Return the address of the relation that is no longer a partition of rel.
 *
 * If concurrent mode is requested, we run in two transactions.  A side-
 * effect is that this command cannot run in a multi-part ALTER TABLE.
 * Currently, that's enforced by the grammar.
 *
 * The strategy for concurrency is to first modify the partition's
 * pg_inherit catalog row to make it visible to everyone that the
 * partition is detached, lock the partition against writes, and commit
 * the transaction; anyone who requests the partition descriptor from
 * that point onwards has to ignore such a partition.  In a second
 * transaction, we wait until all transactions that could have seen the
 * partition as attached are gone, then we remove the rest of partition
 * metadata (pg_inherits and pg_class.relpartbounds).
 */
ObjectAddress
ATExecDetachPartition(List **wqueue, AlteredTableInfo *tab, Relation rel,
					  RangeVar *name, bool concurrent)
{
	Relation	partRel;
	ObjectAddress address;
	Oid			defaultPartOid;

	/*
	 * We must lock the default partition, because detaching this partition
	 * will change its partition constraint.
	 */
	defaultPartOid =
		get_default_oid_from_partdesc(RelationGetPartitionDesc(rel, true));
	if (OidIsValid(defaultPartOid))
	{
		/*
		 * Concurrent detaching when a default partition exists is not
		 * supported. The main problem is that the default partition
		 * constraint would change.  And there's a definitional problem: what
		 * should happen to the tuples that are being inserted that belong to
		 * the partition being detached?  Putting them on the partition being
		 * detached would be wrong, since they'd become "lost" after the
		 * detaching completes but we cannot put them in the default partition
		 * either until we alter its partition constraint.
		 *
		 * I think we could solve this problem if we effected the constraint
		 * change before committing the first transaction.  But the lock would
		 * have to remain AEL and it would cause concurrent query planning to
		 * be blocked, so changing it that way would be even worse.
		 */
		if (concurrent)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot detach partitions concurrently when a default partition exists")));
		LockRelationOid(defaultPartOid, AccessExclusiveLock);
	}

	/*
	 * In concurrent mode, the partition is locked with share-update-exclusive
	 * in the first transaction.  This allows concurrent transactions to be
	 * doing DML to the partition.
	 */
	partRel = table_openrv(name, concurrent ? ShareUpdateExclusiveLock :
						   AccessExclusiveLock);

	/*
	 * Check inheritance conditions and either delete the pg_inherits row (in
	 * non-concurrent mode) or just set the inhdetachpending flag.
	 */
	if (!concurrent)
		RemoveInheritance(partRel, rel, false);
	else
		MarkInheritDetached(partRel, rel);

	/*
	 * Ensure that foreign keys still hold after this detach.  This keeps
	 * locks on the referencing tables, which prevents concurrent transactions
	 * from adding rows that we wouldn't see.  For this to work in concurrent
	 * mode, it is critical that the partition appears as no longer attached
	 * for the RI queries as soon as the first transaction commits.
	 */
	ATDetachCheckNoForeignKeyRefs(partRel);

	/*
	 * Concurrent mode has to work harder; first we add a new constraint to
	 * the partition that matches the partition constraint.  Then we close our
	 * existing transaction, and in a new one wait for all processes to catch
	 * up on the catalog updates we've done so far; at that point we can
	 * complete the operation.
	 */
	if (concurrent)
	{
		Oid			partrelid,
					parentrelid;
		LOCKTAG		tag;
		char	   *parentrelname;
		char	   *partrelname;

		/*
		 * We're almost done now; the only traces that remain are the
		 * pg_inherits tuple and the partition's relpartbounds.  Before we can
		 * remove those, we need to wait until all transactions that know that
		 * this is a partition are gone.
		 */

		/*
		 * Remember relation OIDs to re-acquire them later; and relation names
		 * too, for error messages if something is dropped in between.
		 */
		partrelid = RelationGetRelid(partRel);
		parentrelid = RelationGetRelid(rel);
		parentrelname = MemoryContextStrdup(PortalContext,
											RelationGetRelationName(rel));
		partrelname = MemoryContextStrdup(PortalContext,
										  RelationGetRelationName(partRel));

		/* Invalidate relcache entries for the parent -- must be before close */
		CacheInvalidateRelcache(rel);

		table_close(partRel, NoLock);
		table_close(rel, NoLock);
		tab->rel = NULL;

		/* Make updated catalog entry visible */
		PopActiveSnapshot();
		CommitTransactionCommand();

		StartTransactionCommand();

		/*
		 * Now wait.  This ensures that all queries that were planned
		 * including the partition are finished before we remove the rest of
		 * catalog entries.  We don't need or indeed want to acquire this
		 * lock, though -- that would block later queries.
		 *
		 * We don't need to concern ourselves with waiting for a lock on the
		 * partition itself, since we will acquire AccessExclusiveLock below.
		 */
		SET_LOCKTAG_RELATION(tag, MyDatabaseId, parentrelid);
		WaitForLockersMultiple(list_make1(&tag), AccessExclusiveLock, false);

		/*
		 * Now acquire locks in both relations again.  Note they may have been
		 * removed in the meantime, so care is required.
		 */
		rel = try_relation_open(parentrelid, ShareUpdateExclusiveLock);
		partRel = try_relation_open(partrelid, AccessExclusiveLock);

		/* If the relations aren't there, something bad happened; bail out */
		if (rel == NULL)
		{
			if (partRel != NULL)	/* shouldn't happen */
				elog(WARNING, "dangling partition \"%s\" remains, can't fix",
					 partrelname);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("partitioned table \"%s\" was removed concurrently",
							parentrelname)));
		}
		if (partRel == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("partition \"%s\" was removed concurrently", partrelname)));

		tab->rel = rel;
	}

	/*
	 * Detaching the partition might involve TOAST table access, so ensure we
	 * have a valid snapshot.
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/* Do the final part of detaching */
	DetachPartitionFinalize(rel, partRel, concurrent, defaultPartOid);

	PopActiveSnapshot();

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(partRel));

	/* keep our lock until commit */
	table_close(partRel, NoLock);

	return address;
}

/*
 * Second part of ALTER TABLE .. DETACH.
 *
 * This is separate so that it can be run independently when the second
 * transaction of the concurrent algorithm fails (crash or abort).
 */
static void
DetachPartitionFinalize(Relation rel, Relation partRel, bool concurrent,
						Oid defaultPartOid)
{
	Relation	classRel;
	List	   *fks;
	ListCell   *cell;
	List	   *indexes;
	Datum		new_val[Natts_pg_class];
	bool		new_null[Natts_pg_class],
				new_repl[Natts_pg_class];
	HeapTuple	tuple,
				newtuple;
	Relation	trigrel = NULL;
	List	   *fkoids = NIL;

	if (concurrent)
	{
		/*
		 * We can remove the pg_inherits row now. (In the non-concurrent case,
		 * this was already done).
		 */
		RemoveInheritance(partRel, rel, true);
	}

	/* Drop any triggers that were cloned on creation/attach. */
	DropClonedTriggersFromPartition(RelationGetRelid(partRel));

	/*
	 * Detach any foreign keys that are inherited.  This includes creating
	 * additional action triggers.
	 */
	fks = copyObject(RelationGetFKeyList(partRel));
	if (fks != NIL)
		trigrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * It's possible that the partition being detached has a foreign key that
	 * references a partitioned table.  When that happens, there are multiple
	 * pg_constraint rows for the partition: one points to the partitioned
	 * table itself, while the others point to each of its partitions.  Only
	 * the topmost one is to be considered here; the child constraints must be
	 * left alone, because conceptually those aren't coming from our parent
	 * partitioned table, but from this partition itself.
	 *
	 * We implement this by collecting all the constraint OIDs in a first scan
	 * of the FK array, and skipping in the loop below those constraints whose
	 * parents are listed here.
	 */
	foreach_node(ForeignKeyCacheInfo, fk, fks)
		fkoids = lappend_oid(fkoids, fk->conoid);

	foreach(cell, fks)
	{
		ForeignKeyCacheInfo *fk = lfirst(cell);
		HeapTuple	contup;
		Form_pg_constraint conform;

		contup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(fk->conoid));
		if (!HeapTupleIsValid(contup))
			elog(ERROR, "cache lookup failed for constraint %u", fk->conoid);
		conform = (Form_pg_constraint) GETSTRUCT(contup);

		/*
		 * Consider only inherited foreign keys, and only if their parents
		 * aren't in the list.
		 */
		if (conform->contype != CONSTRAINT_FOREIGN ||
			!OidIsValid(conform->conparentid) ||
			list_member_oid(fkoids, conform->conparentid))
		{
			ReleaseSysCache(contup);
			continue;
		}

		/*
		 * The constraint on this table must be marked no longer a child of
		 * the parent's constraint, as do its check triggers.
		 */
		ConstraintSetParentConstraint(fk->conoid, InvalidOid, InvalidOid);

		/*
		 * Also, look up the partition's "check" triggers corresponding to the
		 * ENFORCED constraint being detached and detach them from the parent
		 * triggers. NOT ENFORCED constraints do not have these triggers;
		 * therefore, this step is not needed.
		 */
		if (fk->conenforced)
		{
			Oid			insertTriggerOid,
						updateTriggerOid;

			GetForeignKeyCheckTriggers(trigrel,
									   fk->conoid, fk->confrelid, fk->conrelid,
									   &insertTriggerOid, &updateTriggerOid);
			Assert(OidIsValid(insertTriggerOid));
			TriggerSetParentTrigger(trigrel, insertTriggerOid, InvalidOid,
									RelationGetRelid(partRel));
			Assert(OidIsValid(updateTriggerOid));
			TriggerSetParentTrigger(trigrel, updateTriggerOid, InvalidOid,
									RelationGetRelid(partRel));
		}

		/*
		 * Lastly, create the action triggers on the referenced table, using
		 * addFkRecurseReferenced, which requires some elaborate setup (so put
		 * it in a separate block).  While at it, if the table is partitioned,
		 * that function will recurse to create the pg_constraint rows and
		 * action triggers for each partition.
		 *
		 * Note there's no need to do addFkConstraint() here, because the
		 * pg_constraint row already exists.
		 */
		{
			Constraint *fkconstraint;
			int			numfks;
			AttrNumber	conkey[INDEX_MAX_KEYS];
			AttrNumber	confkey[INDEX_MAX_KEYS];
			Oid			conpfeqop[INDEX_MAX_KEYS];
			Oid			conppeqop[INDEX_MAX_KEYS];
			Oid			conffeqop[INDEX_MAX_KEYS];
			int			numfkdelsetcols;
			AttrNumber	confdelsetcols[INDEX_MAX_KEYS];
			Relation	refdRel;

			DeconstructFkConstraintRow(contup,
									   &numfks,
									   conkey,
									   confkey,
									   conpfeqop,
									   conppeqop,
									   conffeqop,
									   &numfkdelsetcols,
									   confdelsetcols);

			/* Create a synthetic node we'll use throughout */
			fkconstraint = makeNode(Constraint);
			fkconstraint->contype = CONSTRAINT_FOREIGN;
			fkconstraint->conname = pstrdup(NameStr(conform->conname));
			fkconstraint->deferrable = conform->condeferrable;
			fkconstraint->initdeferred = conform->condeferred;
			fkconstraint->is_enforced = conform->conenforced;
			fkconstraint->skip_validation = true;
			fkconstraint->initially_valid = conform->convalidated;
			/* a few irrelevant fields omitted here */
			fkconstraint->pktable = NULL;
			fkconstraint->fk_attrs = NIL;
			fkconstraint->pk_attrs = NIL;
			fkconstraint->fk_matchtype = conform->confmatchtype;
			fkconstraint->fk_upd_action = conform->confupdtype;
			fkconstraint->fk_del_action = conform->confdeltype;
			fkconstraint->fk_del_set_cols = NIL;
			fkconstraint->old_conpfeqop = NIL;
			fkconstraint->old_pktable_oid = InvalidOid;
			fkconstraint->location = -1;

			/* set up colnames, used to generate the constraint name */
			for (int i = 0; i < numfks; i++)
			{
				Form_pg_attribute att;

				att = TupleDescAttr(RelationGetDescr(partRel),
									conkey[i] - 1);

				fkconstraint->fk_attrs = lappend(fkconstraint->fk_attrs,
												 makeString(NameStr(att->attname)));
			}

			refdRel = table_open(fk->confrelid, ShareRowExclusiveLock);

			addFkRecurseReferenced(fkconstraint, partRel,
								   refdRel,
								   conform->conindid,
								   fk->conoid,
								   numfks,
								   confkey,
								   conkey,
								   conpfeqop,
								   conppeqop,
								   conffeqop,
								   numfkdelsetcols,
								   confdelsetcols,
								   true,
								   InvalidOid, InvalidOid,
								   conform->conperiod);
			table_close(refdRel, NoLock);	/* keep lock till end of xact */
		}

		ReleaseSysCache(contup);
	}
	list_free_deep(fks);
	if (trigrel)
		table_close(trigrel, RowExclusiveLock);

	/*
	 * Any sub-constraints that are in the referenced-side of a larger
	 * constraint have to be removed.  This partition is no longer part of the
	 * key space of the constraint.
	 */
	foreach(cell, GetParentedForeignKeyRefs(partRel))
	{
		Oid			constrOid = lfirst_oid(cell);
		ObjectAddress constraint;

		ConstraintSetParentConstraint(constrOid, InvalidOid, InvalidOid);
		deleteDependencyRecordsForClass(ConstraintRelationId,
										constrOid,
										ConstraintRelationId,
										DEPENDENCY_INTERNAL);
		CommandCounterIncrement();

		ObjectAddressSet(constraint, ConstraintRelationId, constrOid);
		performDeletion(&constraint, DROP_RESTRICT, 0);
	}

	/* Now we can detach indexes */
	indexes = RelationGetIndexList(partRel);
	foreach(cell, indexes)
	{
		Oid			idxid = lfirst_oid(cell);
		Oid			parentidx;
		Relation	idx;
		Oid			constrOid;
		Oid			parentConstrOid;

		if (!has_superclass(idxid))
			continue;

		parentidx = get_partition_parent(idxid, false);
		Assert((IndexGetRelation(parentidx, false) == RelationGetRelid(rel)));

		idx = index_open(idxid, AccessExclusiveLock);
		IndexSetParentIndex(idx, InvalidOid);

		/*
		 * If there's a constraint associated with the index, detach it too.
		 * Careful: it is possible for a constraint index in a partition to be
		 * the child of a non-constraint index, so verify whether the parent
		 * index does actually have a constraint.
		 */
		constrOid = get_relation_idx_constraint_oid(RelationGetRelid(partRel),
													idxid);
		parentConstrOid = get_relation_idx_constraint_oid(RelationGetRelid(rel),
														  parentidx);
		if (OidIsValid(parentConstrOid) && OidIsValid(constrOid))
			ConstraintSetParentConstraint(constrOid, InvalidOid, InvalidOid);

		index_close(idx, NoLock);
	}

	/* Update pg_class tuple */
	classRel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(partRel)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(partRel));
	Assert(((Form_pg_class) GETSTRUCT(tuple))->relispartition);

	/* Clear relpartbound and reset relispartition */
	memset(new_val, 0, sizeof(new_val));
	memset(new_null, false, sizeof(new_null));
	memset(new_repl, false, sizeof(new_repl));
	new_val[Anum_pg_class_relpartbound - 1] = (Datum) 0;
	new_null[Anum_pg_class_relpartbound - 1] = true;
	new_repl[Anum_pg_class_relpartbound - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(classRel),
								 new_val, new_null, new_repl);

	((Form_pg_class) GETSTRUCT(newtuple))->relispartition = false;
	CatalogTupleUpdate(classRel, &newtuple->t_self, newtuple);
	heap_freetuple(newtuple);
	table_close(classRel, RowExclusiveLock);

	/*
	 * Drop identity property from all identity columns of partition.
	 */
	for (int attno = 0; attno < RelationGetNumberOfAttributes(partRel); attno++)
	{
		Form_pg_attribute attr = TupleDescAttr(partRel->rd_att, attno);

		if (!attr->attisdropped && attr->attidentity)
			ATExecDropIdentity(partRel, NameStr(attr->attname), false,
							   AccessExclusiveLock, true, true);
	}

	if (OidIsValid(defaultPartOid))
	{
		/*
		 * If the relation being detached is the default partition itself,
		 * remove it from the parent's pg_partitioned_table entry.
		 *
		 * If not, we must invalidate default partition's relcache entry, as
		 * in StorePartitionBound: its partition constraint depends on every
		 * other partition's partition constraint.
		 */
		if (RelationGetRelid(partRel) == defaultPartOid)
			update_default_partition_oid(RelationGetRelid(rel), InvalidOid);
		else
			CacheInvalidateRelcacheByRelid(defaultPartOid);
	}

	/*
	 * Invalidate the parent's relcache so that the partition is no longer
	 * included in its partition descriptor.
	 */
	CacheInvalidateRelcache(rel);

	/*
	 * If the partition we just detached is partitioned itself, invalidate
	 * relcache for all descendent partitions too to ensure that their
	 * rd_partcheck expression trees are rebuilt; must lock partitions before
	 * doing so, using the same lockmode as what partRel has been locked with
	 * by the caller.
	 */
	if (partRel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		List	   *children;

		children = find_all_inheritors(RelationGetRelid(partRel),
									   AccessExclusiveLock, NULL);
		foreach(cell, children)
		{
			CacheInvalidateRelcacheByRelid(lfirst_oid(cell));
		}
	}
}

/*
 * ALTER TABLE ... DETACH PARTITION ... FINALIZE
 *
 * To use when a DETACH PARTITION command previously did not run to
 * completion; this completes the detaching process.
 */
ObjectAddress
ATExecDetachPartitionFinalize(Relation rel, RangeVar *name)
{
	Relation	partRel;
	ObjectAddress address;
	Snapshot	snap = GetActiveSnapshot();

	partRel = table_openrv(name, AccessExclusiveLock);

	/*
	 * Wait until existing snapshots are gone.  This is important if the
	 * second transaction of DETACH PARTITION CONCURRENTLY is canceled: the
	 * user could immediately run DETACH FINALIZE without actually waiting for
	 * existing transactions.  We must not complete the detach action until
	 * all such queries are complete (otherwise we would present them with an
	 * inconsistent view of catalogs).
	 */
	WaitForOlderSnapshots(snap->xmin, false);

	DetachPartitionFinalize(rel, partRel, true, InvalidOid);

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(partRel));

	table_close(partRel, NoLock);

	return address;
}

/*
 * DropClonedTriggersFromPartition
 *		subroutine for ATExecDetachPartition to remove any triggers that were
 *		cloned to the partition when it was created-as-partition or attached.
 *		This undoes what CloneRowTriggersToPartition did.
 */
static void
DropClonedTriggersFromPartition(Oid partitionId)
{
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	trigtup;
	Relation	tgrel;
	ObjectAddresses *objects;

	objects = new_object_addresses();

	/*
	 * Scan pg_trigger to search for all triggers on this rel.
	 */
	ScanKeyInit(&skey, Anum_pg_trigger_tgrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(partitionId));
	tgrel = table_open(TriggerRelationId, RowExclusiveLock);
	scan = systable_beginscan(tgrel, TriggerRelidNameIndexId,
							  true, NULL, 1, &skey);
	while (HeapTupleIsValid(trigtup = systable_getnext(scan)))
	{
		Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(trigtup);
		ObjectAddress trig;

		/* Ignore triggers that weren't cloned */
		if (!OidIsValid(pg_trigger->tgparentid))
			continue;

		/*
		 * Ignore internal triggers that are implementation objects of foreign
		 * keys, because these will be detached when the foreign keys
		 * themselves are.
		 */
		if (OidIsValid(pg_trigger->tgconstrrelid))
			continue;

		/*
		 * This is ugly, but necessary: remove the dependency markings on the
		 * trigger so that it can be removed.
		 */
		deleteDependencyRecordsForClass(TriggerRelationId, pg_trigger->oid,
										TriggerRelationId,
										DEPENDENCY_PARTITION_PRI);
		deleteDependencyRecordsForClass(TriggerRelationId, pg_trigger->oid,
										RelationRelationId,
										DEPENDENCY_PARTITION_SEC);

		/* remember this trigger to remove it below */
		ObjectAddressSet(trig, TriggerRelationId, pg_trigger->oid);
		add_exact_object_address(&trig, objects);
	}

	/* make the dependency removal visible to the deletion below */
	CommandCounterIncrement();
	performMultipleDeletions(objects, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	/* done */
	free_object_addresses(objects);
	systable_endscan(scan);
	table_close(tgrel, RowExclusiveLock);
}

/*
 * Before acquiring lock on an index, acquire the same lock on the owning
 * table.
 */
struct AttachIndexCallbackState
{
	Oid			partitionOid;
	Oid			parentTblOid;
	bool		lockedParentTbl;
};

static void
RangeVarCallbackForAttachIndex(const RangeVar *rv, Oid relOid, Oid oldRelOid,
							   void *arg)
{
	struct AttachIndexCallbackState *state;
	Form_pg_class classform;
	HeapTuple	tuple;

	state = (struct AttachIndexCallbackState *) arg;

	if (!state->lockedParentTbl)
	{
		LockRelationOid(state->parentTblOid, AccessShareLock);
		state->lockedParentTbl = true;
	}

	/*
	 * If we previously locked some other heap, and the name we're looking up
	 * no longer refers to an index on that relation, release the now-useless
	 * lock.  XXX maybe we should do *after* we verify whether the index does
	 * not actually belong to the same relation ...
	 */
	if (relOid != oldRelOid && OidIsValid(state->partitionOid))
	{
		UnlockRelationOid(state->partitionOid, AccessShareLock);
		state->partitionOid = InvalidOid;
	}

	/* Didn't find a relation, so no need for locking or permission checks. */
	if (!OidIsValid(relOid))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(tuple))
		return;					/* concurrently dropped, so nothing to do */
	classform = (Form_pg_class) GETSTRUCT(tuple);
	if (classform->relkind != RELKIND_PARTITIONED_INDEX &&
		classform->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("\"%s\" is not an index", rv->relname)));
	ReleaseSysCache(tuple);

	/*
	 * Since we need only examine the heap's tupledesc, an access share lock
	 * on it (preventing any DDL) is sufficient.
	 */
	state->partitionOid = IndexGetRelation(relOid, false);
	LockRelationOid(state->partitionOid, AccessShareLock);
}

/*
 * ALTER INDEX i1 ATTACH PARTITION i2
 */
ObjectAddress
ATExecAttachPartitionIdx(List **wqueue, Relation parentIdx, RangeVar *name)
{
	Relation	partIdx;
	Relation	partTbl;
	Relation	parentTbl;
	ObjectAddress address;
	Oid			partIdxId;
	Oid			currParent;
	struct AttachIndexCallbackState state;

	/*
	 * We need to obtain lock on the index 'name' to modify it, but we also
	 * need to read its owning table's tuple descriptor -- so we need to lock
	 * both.  To avoid deadlocks, obtain lock on the table before doing so on
	 * the index.  Furthermore, we need to examine the parent table of the
	 * partition, so lock that one too.
	 */
	state.partitionOid = InvalidOid;
	state.parentTblOid = parentIdx->rd_index->indrelid;
	state.lockedParentTbl = false;
	partIdxId =
		RangeVarGetRelidExtended(name, AccessExclusiveLock, 0,
								 RangeVarCallbackForAttachIndex,
								 &state);
	/* Not there? */
	if (!OidIsValid(partIdxId))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" does not exist", name->relname)));

	/* no deadlock risk: RangeVarGetRelidExtended already acquired the lock */
	partIdx = relation_open(partIdxId, AccessExclusiveLock);

	/* we already hold locks on both tables, so this is safe: */
	parentTbl = relation_open(parentIdx->rd_index->indrelid, AccessShareLock);
	partTbl = relation_open(partIdx->rd_index->indrelid, NoLock);

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(partIdx));

	/* Silently do nothing if already in the right state */
	currParent = partIdx->rd_rel->relispartition ?
		get_partition_parent(partIdxId, false) : InvalidOid;
	if (currParent != RelationGetRelid(parentIdx))
	{
		IndexInfo  *childInfo;
		IndexInfo  *parentInfo;
		AttrMap    *attmap;
		bool		found;
		int			i;
		PartitionDesc partDesc;
		Oid			constraintOid,
					cldConstrId = InvalidOid;

		/*
		 * If this partition already has an index attached, refuse the
		 * operation.
		 */
		refuseDupeIndexAttach(parentIdx, partIdx, partTbl);

		if (OidIsValid(currParent))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
							RelationGetRelationName(partIdx),
							RelationGetRelationName(parentIdx)),
					 errdetail("Index \"%s\" is already attached to another index.",
							   RelationGetRelationName(partIdx))));

		/* Make sure it indexes a partition of the other index's table */
		partDesc = RelationGetPartitionDesc(parentTbl, true);
		found = false;
		for (i = 0; i < partDesc->nparts; i++)
		{
			if (partDesc->oids[i] == state.partitionOid)
			{
				found = true;
				break;
			}
		}
		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
							RelationGetRelationName(partIdx),
							RelationGetRelationName(parentIdx)),
					 errdetail("Index \"%s\" is not an index on any partition of table \"%s\".",
							   RelationGetRelationName(partIdx),
							   RelationGetRelationName(parentTbl))));

		/* Ensure the indexes are compatible */
		childInfo = BuildIndexInfo(partIdx);
		parentInfo = BuildIndexInfo(parentIdx);
		attmap = build_attrmap_by_name(RelationGetDescr(partTbl),
									   RelationGetDescr(parentTbl),
									   false);
		if (!CompareIndexInfo(childInfo, parentInfo,
							  partIdx->rd_indcollation,
							  parentIdx->rd_indcollation,
							  partIdx->rd_opfamily,
							  parentIdx->rd_opfamily,
							  attmap))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
							RelationGetRelationName(partIdx),
							RelationGetRelationName(parentIdx)),
					 errdetail("The index definitions do not match.")));

		/*
		 * If there is a constraint in the parent, make sure there is one in
		 * the child too.
		 */
		constraintOid = get_relation_idx_constraint_oid(RelationGetRelid(parentTbl),
														RelationGetRelid(parentIdx));

		if (OidIsValid(constraintOid))
		{
			cldConstrId = get_relation_idx_constraint_oid(RelationGetRelid(partTbl),
														  partIdxId);
			if (!OidIsValid(cldConstrId))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
								RelationGetRelationName(partIdx),
								RelationGetRelationName(parentIdx)),
						 errdetail("The index \"%s\" belongs to a constraint in table \"%s\" but no constraint exists for index \"%s\".",
								   RelationGetRelationName(parentIdx),
								   RelationGetRelationName(parentTbl),
								   RelationGetRelationName(partIdx))));
		}

		/*
		 * If it's a primary key, make sure the columns in the partition are
		 * NOT NULL.
		 */
		if (parentIdx->rd_index->indisprimary)
			verifyPartitionIndexNotNull(childInfo, partTbl);

		/* All good -- do it */
		IndexSetParentIndex(partIdx, RelationGetRelid(parentIdx));
		if (OidIsValid(constraintOid))
			ConstraintSetParentConstraint(cldConstrId, constraintOid,
										  RelationGetRelid(partTbl));

		free_attrmap(attmap);

		validatePartitionedIndex(parentIdx, parentTbl);
	}

	relation_close(parentTbl, AccessShareLock);
	/* keep these locks till commit */
	relation_close(partTbl, NoLock);
	relation_close(partIdx, NoLock);

	return address;
}

/*
 * Verify whether the given partition already contains an index attached
 * to the given partitioned index.  If so, raise an error.
 */
static void
refuseDupeIndexAttach(Relation parentIdx, Relation partIdx, Relation partitionTbl)
{
	Oid			existingIdx;

	existingIdx = index_get_partition(partitionTbl,
									  RelationGetRelid(parentIdx));
	if (OidIsValid(existingIdx))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
						RelationGetRelationName(partIdx),
						RelationGetRelationName(parentIdx)),
				 errdetail("Another index \"%s\" is already attached for partition \"%s\".",
						   get_rel_name(existingIdx),
						   RelationGetRelationName(partitionTbl))));
}

/*
 * Verify whether the set of attached partition indexes to a parent index on
 * a partitioned table is complete.  If it is, mark the parent index valid.
 *
 * This should be called each time a partition index is attached.
 */
static void
validatePartitionedIndex(Relation partedIdx, Relation partedTbl)
{
	Relation	inheritsRel;
	SysScanDesc scan;
	ScanKeyData key;
	int			tuples = 0;
	HeapTuple	inhTup;
	bool		updated = false;

	Assert(partedIdx->rd_rel->relkind == RELKIND_PARTITIONED_INDEX);

	/*
	 * Scan pg_inherits for this parent index.  Count each valid index we find
	 * (verifying the pg_index entry for each), and if we reach the total
	 * amount we expect, we can mark this parent index as valid.
	 */
	inheritsRel = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(partedIdx)));
	scan = systable_beginscan(inheritsRel, InheritsParentIndexId, true,
							  NULL, 1, &key);
	while ((inhTup = systable_getnext(scan)) != NULL)
	{
		Form_pg_inherits inhForm = (Form_pg_inherits) GETSTRUCT(inhTup);
		HeapTuple	indTup;
		Form_pg_index indexForm;

		indTup = SearchSysCache1(INDEXRELID,
								 ObjectIdGetDatum(inhForm->inhrelid));
		if (!HeapTupleIsValid(indTup))
			elog(ERROR, "cache lookup failed for index %u", inhForm->inhrelid);
		indexForm = (Form_pg_index) GETSTRUCT(indTup);
		if (indexForm->indisvalid)
			tuples += 1;
		ReleaseSysCache(indTup);
	}

	/* Done with pg_inherits */
	systable_endscan(scan);
	table_close(inheritsRel, AccessShareLock);

	/*
	 * If we found as many inherited indexes as the partitioned table has
	 * partitions, we're good; update pg_index to set indisvalid.
	 */
	if (tuples == RelationGetPartitionDesc(partedTbl, true)->nparts)
	{
		Relation	idxRel;
		HeapTuple	indTup;
		Form_pg_index indexForm;

		idxRel = table_open(IndexRelationId, RowExclusiveLock);
		indTup = SearchSysCacheCopy1(INDEXRELID,
									 ObjectIdGetDatum(RelationGetRelid(partedIdx)));
		if (!HeapTupleIsValid(indTup))
			elog(ERROR, "cache lookup failed for index %u",
				 RelationGetRelid(partedIdx));
		indexForm = (Form_pg_index) GETSTRUCT(indTup);

		indexForm->indisvalid = true;
		updated = true;

		CatalogTupleUpdate(idxRel, &indTup->t_self, indTup);

		table_close(idxRel, RowExclusiveLock);
		heap_freetuple(indTup);
	}

	/*
	 * If this index is in turn a partition of a larger index, validating it
	 * might cause the parent to become valid also.  Try that.
	 */
	if (updated && partedIdx->rd_rel->relispartition)
	{
		Oid			parentIdxId,
					parentTblId;
		Relation	parentIdx,
					parentTbl;

		/* make sure we see the validation we just did */
		CommandCounterIncrement();

		parentIdxId = get_partition_parent(RelationGetRelid(partedIdx), false);
		parentTblId = get_partition_parent(RelationGetRelid(partedTbl), false);
		parentIdx = relation_open(parentIdxId, AccessExclusiveLock);
		parentTbl = relation_open(parentTblId, AccessExclusiveLock);
		Assert(!parentIdx->rd_index->indisvalid);

		validatePartitionedIndex(parentIdx, parentTbl);

		relation_close(parentIdx, AccessExclusiveLock);
		relation_close(parentTbl, AccessExclusiveLock);
	}
}

/*
 * When attaching an index as a partition of a partitioned index which is a
 * primary key, verify that all the columns in the partition are marked NOT
 * NULL.
 */
static void
verifyPartitionIndexNotNull(IndexInfo *iinfo, Relation partition)
{
	for (int i = 0; i < iinfo->ii_NumIndexKeyAttrs; i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(partition),
											  iinfo->ii_IndexAttrNumbers[i] - 1);

		if (!att->attnotnull)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					errmsg("invalid primary key definition"),
					errdetail("Column \"%s\" of relation \"%s\" is not marked NOT NULL.",
							  NameStr(att->attname),
							  RelationGetRelationName(partition)));
	}
}

/*
 * Return an OID list of constraints that reference the given relation
 * that are marked as having a parent constraints.
 */
static List *
GetParentedForeignKeyRefs(Relation partition)
{
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData key[2];
	List	   *constraints = NIL;

	/*
	 * If no indexes, or no columns are referenceable by FKs, we can avoid the
	 * scan.
	 */
	if (RelationGetIndexList(partition) == NIL ||
		bms_is_empty(RelationGetIndexAttrBitmap(partition,
												INDEX_ATTR_BITMAP_KEY)))
		return NIL;

	/* Search for constraints referencing this table */
	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_constraint_confrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(partition)));
	ScanKeyInit(&key[1],
				Anum_pg_constraint_contype, BTEqualStrategyNumber,
				F_CHAREQ, CharGetDatum(CONSTRAINT_FOREIGN));

	/* XXX This is a seqscan, as we don't have a usable index */
	scan = systable_beginscan(pg_constraint, InvalidOid, true, NULL, 2, key);
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/*
		 * We only need to process constraints that are part of larger ones.
		 */
		if (!OidIsValid(constrForm->conparentid))
			continue;

		constraints = lappend_oid(constraints, constrForm->oid);
	}

	systable_endscan(scan);
	table_close(pg_constraint, AccessShareLock);

	return constraints;
}

/*
 * During DETACH PARTITION, verify that any foreign keys pointing to the
 * partitioned table would not become invalid.  An error is raised if any
 * referenced values exist.
 */
static void
ATDetachCheckNoForeignKeyRefs(Relation partition)
{
	List	   *constraints;
	ListCell   *cell;

	constraints = GetParentedForeignKeyRefs(partition);

	foreach(cell, constraints)
	{
		Oid			constrOid = lfirst_oid(cell);
		HeapTuple	tuple;
		Form_pg_constraint constrForm;
		Relation	rel;
		Trigger		trig = {0};

		tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(constrOid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for constraint %u", constrOid);
		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		Assert(OidIsValid(constrForm->conparentid));
		Assert(constrForm->confrelid == RelationGetRelid(partition));

		/* prevent data changes into the referencing table until commit */
		rel = table_open(constrForm->conrelid, ShareLock);

		trig.tgoid = InvalidOid;
		trig.tgname = NameStr(constrForm->conname);
		trig.tgenabled = TRIGGER_FIRES_ON_ORIGIN;
		trig.tgisinternal = true;
		trig.tgconstrrelid = RelationGetRelid(partition);
		trig.tgconstrindid = constrForm->conindid;
		trig.tgconstraint = constrForm->oid;
		trig.tgdeferrable = false;
		trig.tginitdeferred = false;
		/* we needn't fill in remaining fields */

		RI_PartitionRemove_Check(&trig, rel, partition);

		ReleaseSysCache(tuple);

		table_close(rel, NoLock);
	}
}

/*
 * buildExpressionExecutionStates: build the needed expression execution states
 * for new partition (newPartRel) checks and initialize expressions for
 * generated columns. All expressions should be created in "tab"
 * (AlteredTableInfo structure).
 */
static void
buildExpressionExecutionStates(AlteredTableInfo *tab, Relation newPartRel, EState *estate)
{
	/*
	 * Build the needed expression execution states. Here, we expect only NOT
	 * NULL and CHECK constraint.
	 */
	foreach_ptr(NewConstraint, con, tab->constraints)
	{
		switch (con->contype)
		{
			case CONSTR_CHECK:

				/*
				 * We already expanded virtual expression in
				 * createTableConstraints.
				 */
				con->qualstate = ExecPrepareExpr((Expr *) con->qual, estate);
				break;
			case CONSTR_NOTNULL:
				/* Nothing to do here. */
				break;
			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 (int) con->contype);
		}
	}

	/* Expression already planned in createTableConstraints */
	foreach_ptr(NewColumnValue, ex, tab->newvals)
		ex->exprstate = ExecInitExpr((Expr *) ex->expr, NULL);
}

/*
 * evaluateGeneratedExpressionsAndCheckConstraints: evaluate any generated
 * expressions for "tab" (AlteredTableInfo structure) whose inputs come from
 * the new tuple (insertslot) of the new partition (newPartRel).
 */
static void
evaluateGeneratedExpressionsAndCheckConstraints(AlteredTableInfo *tab,
												Relation newPartRel,
												TupleTableSlot *insertslot,
												ExprContext *econtext)
{
	econtext->ecxt_scantuple = insertslot;

	foreach_ptr(NewColumnValue, ex, tab->newvals)
	{
		if (!ex->is_generated)
			continue;

		insertslot->tts_values[ex->attnum - 1]
			= ExecEvalExpr(ex->exprstate,
						   econtext,
						   &insertslot->tts_isnull[ex->attnum - 1]);
	}

	foreach_ptr(NewConstraint, con, tab->constraints)
	{
		switch (con->contype)
		{
			case CONSTR_CHECK:
				if (!ExecCheck(con->qualstate, econtext))
					ereport(ERROR,
							errcode(ERRCODE_CHECK_VIOLATION),
							errmsg("check constraint \"%s\" of relation \"%s\" is violated by some row",
								   con->name, RelationGetRelationName(newPartRel)),
							errtableconstraint(newPartRel, con->name));
				break;
			case CONSTR_NOTNULL:
			case CONSTR_FOREIGN:
				/* Nothing to do here */
				break;
			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 (int) con->contype);
		}
	}
}

/*
 * getAttributesList: build a list of columns (ColumnDef) based on parent_rel
 */
static List *
getAttributesList(Relation parent_rel)
{
	AttrNumber	parent_attno;
	TupleDesc	modelDesc;
	List	   *colList = NIL;

	modelDesc = RelationGetDescr(parent_rel);

	for (parent_attno = 1; parent_attno <= modelDesc->natts;
		 parent_attno++)
	{
		Form_pg_attribute attribute = TupleDescAttr(modelDesc,
													parent_attno - 1);
		ColumnDef  *def;

		/* Ignore dropped columns in the parent. */
		if (attribute->attisdropped)
			continue;

		def = makeColumnDef(NameStr(attribute->attname), attribute->atttypid,
							attribute->atttypmod, attribute->attcollation);

		def->is_not_null = attribute->attnotnull;

		/* Copy identity. */
		def->identity = attribute->attidentity;

		/* Copy attgenerated. */
		def->generated = attribute->attgenerated;

		def->storage = attribute->attstorage;

		/* Likewise, copy compression. */
		if (CompressionMethodIsValid(attribute->attcompression))
			def->compression =
				pstrdup(GetCompressionMethodName(attribute->attcompression));
		else
			def->compression = NULL;

		/* Add to column list. */
		colList = lappend(colList, def);
	}

	return colList;
}

/*
 * createTableConstraints:
 * create check constraints, default values, and generated values for newRel
 * based on parent_rel.  tab is pending-work queue for newRel, we may need it in
 * MergePartitionsMoveRows.
 */
static void
createTableConstraints(List **wqueue, AlteredTableInfo *tab,
					   Relation parent_rel, Relation newRel)
{
	TupleDesc	tupleDesc;
	TupleConstr *constr;
	AttrMap    *attmap;
	AttrNumber	parent_attno;
	int			ccnum;
	List	   *constraints = NIL;
	List	   *cookedConstraints = NIL;

	tupleDesc = RelationGetDescr(parent_rel);
	constr = tupleDesc->constr;

	if (!constr)
		return;

	/*
	 * Construct a map from the parent relation's attnos to the child rel's.
	 * This re-checks type match, etc, although it shouldn't be possible to
	 * have a failure since both tables are locked.
	 */
	attmap = build_attrmap_by_name(RelationGetDescr(newRel),
								   tupleDesc,
								   false);

	/* Cycle for default values. */
	for (parent_attno = 1; parent_attno <= tupleDesc->natts; parent_attno++)
	{
		Form_pg_attribute attribute = TupleDescAttr(tupleDesc,
													parent_attno - 1);

		/* Ignore dropped columns in the parent. */
		if (attribute->attisdropped)
			continue;

		/* Copy the default, if present, and it should be copied. */
		if (attribute->atthasdef)
		{
			Node	   *this_default = NULL;
			bool		found_whole_row;
			AttrNumber	num;
			Node	   *def;
			NewColumnValue *newval;

			if (attribute->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
				this_default = build_generation_expression(parent_rel, attribute->attnum);
			else
			{
				this_default = TupleDescGetDefault(tupleDesc, attribute->attnum);
				if (this_default == NULL)
					elog(ERROR, "default expression not found for attribute %d of relation \"%s\"",
						 attribute->attnum, RelationGetRelationName(parent_rel));
			}

			num = attmap->attnums[parent_attno - 1];
			def = map_variable_attnos(this_default, 1, 0, attmap, InvalidOid, &found_whole_row);

			if (found_whole_row && attribute->attgenerated != '\0')
				elog(ERROR, "cannot convert whole-row table reference");

			/* Add a pre-cooked default expression. */
			StoreAttrDefault(newRel, num, def, true);

			/*
			 * Stored generated column expressions in parent_rel might
			 * reference the tableoid.  newRel, parent_rel tableoid clear is
			 * not the same. If so, these stored generated columns require
			 * recomputation for newRel within MergePartitionsMoveRows.
			 */
			if (attribute->attgenerated == ATTRIBUTE_GENERATED_STORED)
			{
				newval = palloc0_object(NewColumnValue);
				newval->attnum = num;
				newval->expr = expression_planner((Expr *) def);
				newval->is_generated = (attribute->attgenerated != '\0');
				tab->newvals = lappend(tab->newvals, newval);
			}
		}
	}

	/* Cycle for CHECK constraints. */
	for (ccnum = 0; ccnum < constr->num_check; ccnum++)
	{
		char	   *ccname = constr->check[ccnum].ccname;
		char	   *ccbin = constr->check[ccnum].ccbin;
		bool		ccenforced = constr->check[ccnum].ccenforced;
		bool		ccnoinherit = constr->check[ccnum].ccnoinherit;
		bool		ccvalid = constr->check[ccnum].ccvalid;
		Node	   *ccbin_node;
		bool		found_whole_row;
		Constraint *constr;

		/*
		 * The partitioned table can not have a NO INHERIT check constraint
		 * (see StoreRelCheck function for details).
		 */
		Assert(!ccnoinherit);

		ccbin_node = map_variable_attnos(stringToNode(ccbin),
										 1, 0,
										 attmap,
										 InvalidOid, &found_whole_row);

		/*
		 * For the moment we have to reject whole-row variables (as for CREATE
		 * TABLE LIKE and inheritances).
		 */
		if (found_whole_row)
			elog(ERROR, "Constraint \"%s\" contains a whole-row reference to table \"%s\".",
				 ccname,
				 RelationGetRelationName(parent_rel));

		constr = makeNode(Constraint);
		constr->contype = CONSTR_CHECK;
		constr->conname = pstrdup(ccname);
		constr->deferrable = false;
		constr->initdeferred = false;
		constr->is_enforced = ccenforced;
		constr->skip_validation = !ccvalid;
		constr->initially_valid = ccvalid;
		constr->is_no_inherit = ccnoinherit;
		constr->raw_expr = NULL;
		constr->cooked_expr = nodeToString(ccbin_node);
		constr->location = -1;
		constraints = lappend(constraints, constr);
	}

	/* Install all CHECK constraints. */
	cookedConstraints = AddRelationNewConstraints(newRel, NIL, constraints,
												  false, true, true, NULL);

	/* Make the additional catalog changes visible. */
	CommandCounterIncrement();

	/*
	 * parent_rel check constraint expression may reference tableoid, so later
	 * in MergePartitionsMoveRows, we need to evaluate the check constraint
	 * again for the newRel. We can check whether the check constraint
	 * contains a tableoid reference via pull_varattnos.
	 */
	foreach_ptr(CookedConstraint, ccon, cookedConstraints)
	{
		if (!ccon->skip_validation)
		{
			Node	   *qual;
			Bitmapset  *attnums = NULL;

			Assert(ccon->contype == CONSTR_CHECK);
			qual = expand_generated_columns_in_expr(ccon->expr, newRel, 1);
			pull_varattnos(qual, 1, &attnums);

			/*
			 * Add a check only if it contains a tableoid
			 * (TableOidAttributeNumber).
			 */
			if (bms_is_member(TableOidAttributeNumber - FirstLowInvalidHeapAttributeNumber,
							  attnums))
			{
				NewConstraint *newcon;

				newcon = palloc0_object(NewConstraint);
				newcon->name = ccon->name;
				newcon->contype = CONSTR_CHECK;
				newcon->qual = qual;

				tab->constraints = lappend(tab->constraints, newcon);
			}
		}
	}

	/* Don't need the cookedConstraints anymore. */
	list_free_deep(cookedConstraints);

	/* Reproduce not-null constraints. */
	if (constr->has_not_null)
	{
		List	   *nnconstraints;

		/*
		 * The "include_noinh" argument is false because a partitioned table
		 * can't have NO INHERIT constraint.
		 */
		nnconstraints = RelationGetNotNullConstraints(RelationGetRelid(parent_rel),
													  false, false);

		Assert(list_length(nnconstraints) > 0);

		/*
		 * We already set pg_attribute.attnotnull in createPartitionTable. No
		 * need call set_attnotnull again.
		 */
		AddRelationNewConstraints(newRel, NIL, nnconstraints, false, true, true, NULL);
	}
}

/*
 * createPartitionTable:
 *
 * Create a new partition (newPartName) for the partitioned table (parent_rel).
 * ownerId is determined by the partition on which the operation is performed,
 * so it is passed separately.  The new partition will inherit the access method
 * and persistence type from the parent table.
 *
 * Returns the created relation (locked in AccessExclusiveLock mode).
 */
static Relation
createPartitionTable(List **wqueue, RangeVar *newPartName,
					 Relation parent_rel, Oid ownerId)
{
	Relation	newRel;
	Oid			newRelId;
	Oid			existingRelid;
	TupleDesc	descriptor;
	List	   *colList = NIL;
	Oid			relamId;
	Oid			namespaceId;
	AlteredTableInfo *new_partrel_tab;
	Form_pg_class parent_relform = parent_rel->rd_rel;

	/* If the existing rel is temp, it must belong to this session. */
	if (RELATION_IS_OTHER_TEMP(parent_rel))
		ereport(ERROR,
				errcode(ERRCODE_WRONG_OBJECT_TYPE),
				errmsg("cannot create as partition of temporary relation of another session"));

	/* Look up inheritance ancestors and generate the relation schema. */
	colList = getAttributesList(parent_rel);

	/* Create a tuple descriptor from the relation schema. */
	descriptor = BuildDescForRelation(colList);

	/* Look up the access method for the new relation. */
	relamId = (parent_relform->relam != InvalidOid) ? parent_relform->relam : HEAP_TABLE_AM_OID;

	/* Look up the namespace in which we are supposed to create the relation. */
	namespaceId =
		RangeVarGetAndCheckCreationNamespace(newPartName, NoLock, &existingRelid);
	if (OidIsValid(existingRelid))
		ereport(ERROR,
				errcode(ERRCODE_DUPLICATE_TABLE),
				errmsg("relation \"%s\" already exists", newPartName->relname));

	/*
	 * We intended to create the partition with the same persistence as the
	 * parent table, but we still need to recheck because that might be
	 * affected by the search_path.  If the parent is permanent, so must be
	 * all of its partitions.
	 */
	if (parent_relform->relpersistence != RELPERSISTENCE_TEMP &&
		newPartName->relpersistence == RELPERSISTENCE_TEMP)
		ereport(ERROR,
				errcode(ERRCODE_WRONG_OBJECT_TYPE),
				errmsg("cannot create a temporary relation as partition of permanent relation \"%s\"",
					   RelationGetRelationName(parent_rel)));

	/* Permanent rels cannot be partitions belonging to a temporary parent. */
	if (newPartName->relpersistence != RELPERSISTENCE_TEMP &&
		parent_relform->relpersistence == RELPERSISTENCE_TEMP)
		ereport(ERROR,
				errcode(ERRCODE_WRONG_OBJECT_TYPE),
				errmsg("cannot create a permanent relation as partition of temporary relation \"%s\"",
					   RelationGetRelationName(parent_rel)));

	/* Create the relation. */
	newRelId = heap_create_with_catalog(newPartName->relname,
										namespaceId,
										parent_relform->reltablespace,
										InvalidOid,
										InvalidOid,
										InvalidOid,
										ownerId,
										relamId,
										descriptor,
										NIL,
										RELKIND_RELATION,
										newPartName->relpersistence,
										false,
										false,
										ONCOMMIT_NOOP,
										(Datum) 0,
										true,
										allowSystemTableMods,
										true,
										InvalidOid,
										NULL);

	/*
	 * We must bump the command counter to make the newly-created relation
	 * tuple visible for opening.
	 */
	CommandCounterIncrement();

	/*
	 * Open the new partition with no lock, because we already have an
	 * AccessExclusiveLock placed there after creation.
	 */
	newRel = table_open(newRelId, NoLock);

	/* Find or create a work queue entry for the newly created table. */
	new_partrel_tab = ATGetQueueEntry(wqueue, newRel);

	/* Create constraints, default values, and generated values. */
	createTableConstraints(wqueue, new_partrel_tab, parent_rel, newRel);

	/*
	 * Need to call CommandCounterIncrement, so a fresh relcache entry has
	 * newly installed constraint info.
	 */
	CommandCounterIncrement();

	return newRel;
}

/*
 * MergePartitionsMoveRows: scan partitions to be merged (mergingPartitions)
 * of the partitioned table and move rows into the new partition
 * (newPartRel). We also verify check constraints against these rows.
 */
static void
MergePartitionsMoveRows(List **wqueue, List *mergingPartitions, Relation newPartRel)
{
	CommandId	mycid;
	EState	   *estate;
	AlteredTableInfo *tab;
	ListCell   *ltab;

	/* The FSM is empty, so don't bother using it. */
	int			ti_options = TABLE_INSERT_SKIP_FSM;
	BulkInsertState bistate;	/* state of bulk inserts for partition */
	TupleTableSlot *dstslot;

	/* Find the work queue entry for the new partition table: newPartRel. */
	tab = ATGetQueueEntry(wqueue, newPartRel);

	/* Generate the constraint and default execution states. */
	estate = CreateExecutorState();

	buildExpressionExecutionStates(tab, newPartRel, estate);

	mycid = GetCurrentCommandId(true);

	/* Prepare a BulkInsertState for table_tuple_insert. */
	bistate = GetBulkInsertState();

	/* Create the necessary tuple slot. */
	dstslot = table_slot_create(newPartRel, NULL);

	foreach_oid(merging_oid, mergingPartitions)
	{
		ExprContext *econtext;
		TupleTableSlot *srcslot;
		TupleConversionMap *tuple_map;
		TableScanDesc scan;
		MemoryContext oldCxt;
		Snapshot	snapshot;
		Relation	mergingPartition;

		econtext = GetPerTupleExprContext(estate);

		/*
		 * Partition is already locked in the transformPartitionCmdForMerge
		 * function.
		 */
		mergingPartition = table_open(merging_oid, NoLock);

		/* Create a source tuple slot for the partition being merged. */
		srcslot = table_slot_create(mergingPartition, NULL);

		/*
		 * Map computing for moving attributes of the merged partition to the
		 * new partition.
		 */
		tuple_map = convert_tuples_by_name(RelationGetDescr(mergingPartition),
										   RelationGetDescr(newPartRel));

		/* Scan through the rows. */
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		scan = table_beginscan(mergingPartition, snapshot, 0, NULL);

		/*
		 * Switch to per-tuple memory context and reset it for each tuple
		 * produced, so we don't leak memory.
		 */
		oldCxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		while (table_scan_getnextslot(scan, ForwardScanDirection, srcslot))
		{
			TupleTableSlot *insertslot;

			CHECK_FOR_INTERRUPTS();

			if (tuple_map)
			{
				/* Need to use a map to copy attributes. */
				insertslot = execute_attr_map_slot(tuple_map->attrMap, srcslot, dstslot);
			}
			else
			{
				slot_getallattrs(srcslot);

				/* Copy attributes directly. */
				insertslot = dstslot;

				ExecClearTuple(insertslot);

				memcpy(insertslot->tts_values, srcslot->tts_values,
					   sizeof(Datum) * srcslot->tts_nvalid);
				memcpy(insertslot->tts_isnull, srcslot->tts_isnull,
					   sizeof(bool) * srcslot->tts_nvalid);

				ExecStoreVirtualTuple(insertslot);
			}

			/*
			 * Constraints and GENERATED expressions might reference the
			 * tableoid column, so fill tts_tableOid with the desired value.
			 * (We must do this each time, because it gets overwritten with
			 * newrel's OID during storing.)
			 */
			insertslot->tts_tableOid = RelationGetRelid(newPartRel);

			/*
			 * Now, evaluate any generated expressions whose inputs come from
			 * the new tuple.  We assume these columns won't reference each
			 * other, so that there's no ordering dependency.
			 */
			evaluateGeneratedExpressionsAndCheckConstraints(tab, newPartRel,
															insertslot, econtext);

			/* Write the tuple out to the new relation. */
			table_tuple_insert(newPartRel, insertslot, mycid,
							   ti_options, bistate);

			ResetExprContext(econtext);
		}

		MemoryContextSwitchTo(oldCxt);
		table_endscan(scan);
		UnregisterSnapshot(snapshot);

		if (tuple_map)
			free_conversion_map(tuple_map);

		ExecDropSingleTupleTableSlot(srcslot);
		table_close(mergingPartition, NoLock);
	}

	FreeExecutorState(estate);
	ExecDropSingleTupleTableSlot(dstslot);
	FreeBulkInsertState(bistate);

	table_finish_bulk_insert(newPartRel, ti_options);

	/*
	 * We don't need to process this newPartRel since we already processed it
	 * here, so delete the ALTER TABLE queue for it.
	 */
	foreach(ltab, *wqueue)
	{
		tab = (AlteredTableInfo *) lfirst(ltab);
		if (tab->relid == RelationGetRelid(newPartRel))
		{
			*wqueue = list_delete_cell(*wqueue, ltab);
			break;
		}
	}
}

/*
 * detachPartitionTable: detach partition "child_rel" from partitioned table
 * "parent_rel" with default partition identifier "defaultPartOid"
 */
static void
detachPartitionTable(Relation parent_rel, Relation child_rel, Oid defaultPartOid)
{
	/* Remove the pg_inherits row first. */
	RemoveInheritance(child_rel, parent_rel, false);

	/*
	 * Detaching the partition might involve TOAST table access, so ensure we
	 * have a valid snapshot.
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/* Do the final part of detaching. */
	DetachPartitionFinalize(parent_rel, child_rel, false, defaultPartOid);

	PopActiveSnapshot();
}

/*
 * ALTER TABLE <name> MERGE PARTITIONS <partition-list> INTO <partition-name>
 */
void
ATExecMergePartitions(List **wqueue, AlteredTableInfo *tab, Relation rel,
					  PartitionCmd *cmd, AlterTableUtilityContext *context)
{
	Relation	newPartRel;
	List	   *mergingPartitions = NIL;
	Oid			defaultPartOid;
	Oid			existingRelid;
	Oid			ownerId = InvalidOid;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;

	/*
	 * Check ownership of merged partitions - partitions with different owners
	 * cannot be merged. Also, collect the OIDs of these partitions during the
	 * check.
	 */
	foreach_node(RangeVar, name, cmd->partlist)
	{
		Relation	mergingPartition;

		/*
		 * We are going to detach and remove this partition.  We already took
		 * AccessExclusiveLock lock on transformPartitionCmdForMerge, so here,
		 * NoLock is fine.
		 */
		mergingPartition = table_openrv_extended(name, NoLock, false);
		Assert(CheckRelationLockedByMe(mergingPartition, AccessExclusiveLock, false));

		if (OidIsValid(ownerId))
		{
			/* Do the partitions being merged have different owners? */
			if (ownerId != mergingPartition->rd_rel->relowner)
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("partitions being merged have different owners"));
		}
		else
			ownerId = mergingPartition->rd_rel->relowner;

		/* Store the next merging partition into the list. */
		mergingPartitions = lappend_oid(mergingPartitions,
										RelationGetRelid(mergingPartition));

		table_close(mergingPartition, NoLock);
	}

	/* Look up the existing relation by the new partition name. */
	RangeVarGetAndCheckCreationNamespace(cmd->name, NoLock, &existingRelid);

	/*
	 * Check if this name is already taken.  This helps us to detect the
	 * situation when one of the merging partitions has the same name as the
	 * new partition.  Otherwise, this would fail later on anyway, but
	 * catching this here allows us to emit a nicer error message.
	 */
	if (OidIsValid(existingRelid))
	{
		if (list_member_oid(mergingPartitions, existingRelid))
		{
			/*
			 * The new partition has the same name as one of the merging
			 * partitions.
			 */
			char		tmpRelName[NAMEDATALEN];

			/* Generate a temporary name. */
			sprintf(tmpRelName, "merge-%u-%X-tmp", RelationGetRelid(rel), MyProcPid);

			/*
			 * Rename the existing partition with a temporary name, leaving it
			 * free for the new partition.  We don't need to care about this
			 * in the future because we're going to eventually drop the
			 * existing partition anyway.
			 */
			RenameRelationInternal(existingRelid, tmpRelName, true, false);

			/*
			 * We must bump the command counter to make the new partition
			 * tuple visible for rename.
			 */
			CommandCounterIncrement();
		}
		else
		{
			ereport(ERROR,
					errcode(ERRCODE_DUPLICATE_TABLE),
					errmsg("relation \"%s\" already exists", cmd->name->relname));
		}
	}

	defaultPartOid =
		get_default_oid_from_partdesc(RelationGetPartitionDesc(rel, true));

	/* Detach all merging partitions. */
	foreach_oid(mergingPartitionOid, mergingPartitions)
	{
		Relation	child_rel;

		child_rel = table_open(mergingPartitionOid, NoLock);

		detachPartitionTable(rel, child_rel, defaultPartOid);

		table_close(child_rel, NoLock);
	}

	/*
	 * Perform a preliminary check to determine whether it's safe to drop all
	 * merging partitions before we actually do so later. After merging rows
	 * into the new partitions via MergePartitionsMoveRows, all old partitions
	 * need to be dropped. However, since the drop behavior is DROP_RESTRICT
	 * and the merge process (MergePartitionsMoveRows) can be time-consuming,
	 * performing an early check on the drop eligibility of old partitions is
	 * preferable.
	 */
	foreach_oid(mergingPartitionOid, mergingPartitions)
	{
		ObjectAddress object;

		/* Get oid of the later to be dropped relation. */
		object.objectId = mergingPartitionOid;
		object.classId = RelationRelationId;
		object.objectSubId = 0;

		performDeletionCheck(&object, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
	}

	/*
	 * Create a table for the new partition, using the partitioned table as a
	 * model.
	 */
	Assert(OidIsValid(ownerId));
	newPartRel = createPartitionTable(wqueue, cmd->name, rel, ownerId);

	/*
	 * Switch to the table owner's userid, so that any index functions are run
	 * as that user.  Also, lockdown security-restricted operations and
	 * arrange to make GUC variable changes local to this command.
	 *
	 * Need to do it after determining the namespace in the
	 * createPartitionTable() call.
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(ownerId,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();

	/* Copy data from merged partitions to the new partition. */
	MergePartitionsMoveRows(wqueue, mergingPartitions, newPartRel);

	/* Drop the current partitions before attaching the new one. */
	foreach_oid(mergingPartitionOid, mergingPartitions)
	{
		ObjectAddress object;

		object.objectId = mergingPartitionOid;
		object.classId = RelationRelationId;
		object.objectSubId = 0;

		performDeletion(&object, DROP_RESTRICT, 0);
	}

	list_free(mergingPartitions);

	/*
	 * Attach a new partition to the partitioned table. wqueue = NULL:
	 * verification for each cloned constraint is not needed.
	 */
	attachPartitionTable(NULL, rel, newPartRel, cmd->bound);

	/* Keep the lock until commit. */
	table_close(newPartRel, NoLock);

	/* Roll back any GUC changes executed by index functions. */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore the userid and security context. */
	SetUserIdAndSecContext(save_userid, save_sec_context);
}

/*
 * Struct with the context of the new partition for inserting rows from the
 * split partition.
 */
typedef struct SplitPartitionContext
{
	ExprState  *partqualstate;	/* expression for checking a slot for a
								 * partition (NULL for DEFAULT partition) */
	BulkInsertState bistate;	/* state of bulk inserts for partition */
	TupleTableSlot *dstslot;	/* slot for inserting row into partition */
	AlteredTableInfo *tab;		/* structure with generated column expressions
								 * and check constraint expressions. */
	Relation	partRel;		/* relation for partition */
} SplitPartitionContext;

/*
 * createSplitPartitionContext: create context for partition and fill it
 */
static SplitPartitionContext *
createSplitPartitionContext(Relation partRel)
{
	SplitPartitionContext *pc;

	pc = palloc0_object(SplitPartitionContext);
	pc->partRel = partRel;

	/*
	 * Prepare a BulkInsertState for table_tuple_insert. The FSM is empty, so
	 * don't bother using it.
	 */
	pc->bistate = GetBulkInsertState();

	/* Create a destination tuple slot for the new partition. */
	pc->dstslot = table_slot_create(pc->partRel, NULL);

	return pc;
}

/*
 * deleteSplitPartitionContext: delete context for partition
 */
static void
deleteSplitPartitionContext(SplitPartitionContext *pc, List **wqueue, int ti_options)
{
	ListCell   *ltab;

	ExecDropSingleTupleTableSlot(pc->dstslot);
	FreeBulkInsertState(pc->bistate);

	table_finish_bulk_insert(pc->partRel, ti_options);

	/*
	 * We don't need to process this pc->partRel so delete the ALTER TABLE
	 * queue of it.
	 */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);

		if (tab->relid == RelationGetRelid(pc->partRel))
		{
			*wqueue = list_delete_cell(*wqueue, ltab);
			break;
		}
	}

	pfree(pc);
}

/*
 * SplitPartitionMoveRows: scan split partition (splitRel) of partitioned table
 * (rel) and move rows into new partitions.
 *
 * New partitions description:
 * partlist: list of pointers to SinglePartitionSpec structures.  It contains
 * the partition specification details for all new partitions.
 * newPartRels: list of Relations, new partitions created in
 * ATExecSplitPartition.
 */
static void
SplitPartitionMoveRows(List **wqueue, Relation rel, Relation splitRel,
					   List *partlist, List *newPartRels)
{
	/* The FSM is empty, so don't bother using it. */
	int			ti_options = TABLE_INSERT_SKIP_FSM;
	CommandId	mycid;
	EState	   *estate;
	ListCell   *listptr,
			   *listptr2;
	TupleTableSlot *srcslot;
	ExprContext *econtext;
	TableScanDesc scan;
	Snapshot	snapshot;
	MemoryContext oldCxt;
	List	   *partContexts = NIL;
	TupleConversionMap *tuple_map;
	SplitPartitionContext *defaultPartCtx = NULL,
			   *pc;

	mycid = GetCurrentCommandId(true);

	estate = CreateExecutorState();

	forboth(listptr, partlist, listptr2, newPartRels)
	{
		SinglePartitionSpec *sps = (SinglePartitionSpec *) lfirst(listptr);

		pc = createSplitPartitionContext((Relation) lfirst(listptr2));

		/* Find the work queue entry for the new partition table: newPartRel. */
		pc->tab = ATGetQueueEntry(wqueue, pc->partRel);

		buildExpressionExecutionStates(pc->tab, pc->partRel, estate);

		if (sps->bound->is_default)
		{
			/*
			 * We should not create a structure to check the partition
			 * constraint for the new DEFAULT partition.
			 */
			defaultPartCtx = pc;
		}
		else
		{
			List	   *partConstraint;

			/* Build expression execution states for partition check quals. */
			partConstraint = get_qual_from_partbound(rel, sps->bound);
			partConstraint =
				(List *) eval_const_expressions(NULL,
												(Node *) partConstraint);
			/* Make a boolean expression for ExecCheck(). */
			partConstraint = list_make1(make_ands_explicit(partConstraint));

			/*
			 * Map the vars in the constraint expression from rel's attnos to
			 * splitRel's.
			 */
			partConstraint = map_partition_varattnos(partConstraint,
													 1, splitRel, rel);

			pc->partqualstate =
				ExecPrepareExpr((Expr *) linitial(partConstraint), estate);
			Assert(pc->partqualstate != NULL);
		}

		/* Store partition context into a list. */
		partContexts = lappend(partContexts, pc);
	}

	econtext = GetPerTupleExprContext(estate);

	/* Create the necessary tuple slot. */
	srcslot = table_slot_create(splitRel, NULL);

	/*
	 * Map computing for moving attributes of the split partition to the new
	 * partition (for the first new partition, but other new partitions can
	 * use the same map).
	 */
	pc = (SplitPartitionContext *) lfirst(list_head(partContexts));
	tuple_map = convert_tuples_by_name(RelationGetDescr(splitRel),
									   RelationGetDescr(pc->partRel));

	/* Scan through the rows. */
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = table_beginscan(splitRel, snapshot, 0, NULL);

	/*
	 * Switch to per-tuple memory context and reset it for each tuple
	 * produced, so we don't leak memory.
	 */
	oldCxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

	while (table_scan_getnextslot(scan, ForwardScanDirection, srcslot))
	{
		bool		found = false;
		TupleTableSlot *insertslot;

		CHECK_FOR_INTERRUPTS();

		econtext->ecxt_scantuple = srcslot;

		/* Search partition for the current slot, srcslot. */
		foreach(listptr, partContexts)
		{
			pc = (SplitPartitionContext *) lfirst(listptr);

			/* skip DEFAULT partition */
			if (pc->partqualstate && ExecCheck(pc->partqualstate, econtext))
			{
				found = true;
				break;
			}
		}
		if (!found)
		{
			/* Use the DEFAULT partition if it exists. */
			if (defaultPartCtx)
				pc = defaultPartCtx;
			else
				ereport(ERROR,
						errcode(ERRCODE_CHECK_VIOLATION),
						errmsg("can not find partition for split partition row"),
						errtable(splitRel));
		}

		if (tuple_map)
		{
			/* Need to use a map to copy attributes. */
			insertslot = execute_attr_map_slot(tuple_map->attrMap, srcslot, pc->dstslot);
		}
		else
		{
			/* Extract data from the old tuple. */
			slot_getallattrs(srcslot);

			/* Copy attributes directly. */
			insertslot = pc->dstslot;

			ExecClearTuple(insertslot);

			memcpy(insertslot->tts_values, srcslot->tts_values,
				   sizeof(Datum) * srcslot->tts_nvalid);
			memcpy(insertslot->tts_isnull, srcslot->tts_isnull,
				   sizeof(bool) * srcslot->tts_nvalid);

			ExecStoreVirtualTuple(insertslot);
		}

		/*
		 * Constraints and GENERATED expressions might reference the tableoid
		 * column, so fill tts_tableOid with the desired value. (We must do
		 * this each time, because it gets overwritten with newrel's OID
		 * during storing.)
		 */
		insertslot->tts_tableOid = RelationGetRelid(pc->partRel);

		/*
		 * Now, evaluate any generated expressions whose inputs come from the
		 * new tuple.  We assume these columns won't reference each other, so
		 * that there's no ordering dependency.
		 */
		evaluateGeneratedExpressionsAndCheckConstraints(pc->tab, pc->partRel,
														insertslot, econtext);

		/* Write the tuple out to the new relation. */
		table_tuple_insert(pc->partRel, insertslot, mycid,
						   ti_options, pc->bistate);

		ResetExprContext(econtext);
	}

	MemoryContextSwitchTo(oldCxt);

	table_endscan(scan);
	UnregisterSnapshot(snapshot);

	if (tuple_map)
		free_conversion_map(tuple_map);

	ExecDropSingleTupleTableSlot(srcslot);

	FreeExecutorState(estate);

	foreach_ptr(SplitPartitionContext, spc, partContexts)
		deleteSplitPartitionContext(spc, wqueue, ti_options);
}

/*
 * ALTER TABLE <name> SPLIT PARTITION <partition-name> INTO <partition-list>
 */
void
ATExecSplitPartition(List **wqueue, AlteredTableInfo *tab, Relation rel,
					 PartitionCmd *cmd, AlterTableUtilityContext *context)
{
	Relation	splitRel;
	Oid			splitRelOid;
	ListCell   *listptr,
			   *listptr2;
	bool		isSameName = false;
	char		tmpRelName[NAMEDATALEN];
	List	   *newPartRels = NIL;
	ObjectAddress object;
	Oid			defaultPartOid;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;

	defaultPartOid = get_default_oid_from_partdesc(RelationGetPartitionDesc(rel, true));

	/*
	 * Partition is already locked in the transformPartitionCmdForSplit
	 * function.
	 */
	splitRel = table_openrv(cmd->name, NoLock);

	splitRelOid = RelationGetRelid(splitRel);

	/* Check descriptions of new partitions. */
	foreach_node(SinglePartitionSpec, sps, cmd->partlist)
	{
		Oid			existingRelid;

		/* Look up the existing relation by the new partition name. */
		RangeVarGetAndCheckCreationNamespace(sps->name, NoLock, &existingRelid);

		/*
		 * This would fail later on anyway if the relation already exists. But
		 * by catching it here, we can emit a nicer error message.
		 */
		if (existingRelid == splitRelOid && !isSameName)
			/* One new partition can have the same name as a split partition. */
			isSameName = true;
		else if (OidIsValid(existingRelid))
			ereport(ERROR,
					errcode(ERRCODE_DUPLICATE_TABLE),
					errmsg("relation \"%s\" already exists", sps->name->relname));
	}

	/* Detach the split partition. */
	detachPartitionTable(rel, splitRel, defaultPartOid);

	/*
	 * Perform a preliminary check to determine whether it's safe to drop the
	 * split partition before we actually do so later. After merging rows into
	 * the new partitions via SplitPartitionMoveRows, all old partitions need
	 * to be dropped. However, since the drop behavior is DROP_RESTRICT and
	 * the merge process (SplitPartitionMoveRows) can be time-consuming,
	 * performing an early check on the drop eligibility of old partitions is
	 * preferable.
	 */
	object.objectId = splitRelOid;
	object.classId = RelationRelationId;
	object.objectSubId = 0;
	performDeletionCheck(&object, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	/*
	 * If a new partition has the same name as the split partition, then we
	 * should rename the split partition to reuse its name.
	 */
	if (isSameName)
	{
		/*
		 * We must bump the command counter to make the split partition tuple
		 * visible for renaming.
		 */
		CommandCounterIncrement();
		/* Rename partition. */
		sprintf(tmpRelName, "split-%u-%X-tmp", RelationGetRelid(rel), MyProcPid);
		RenameRelationInternal(splitRelOid, tmpRelName, true, false);

		/*
		 * We must bump the command counter to make the split partition tuple
		 * visible after renaming.
		 */
		CommandCounterIncrement();
	}

	/* Create new partitions (like a split partition), without indexes. */
	foreach_node(SinglePartitionSpec, sps, cmd->partlist)
	{
		Relation	newPartRel;

		newPartRel = createPartitionTable(wqueue, sps->name, rel,
										  splitRel->rd_rel->relowner);
		newPartRels = lappend(newPartRels, newPartRel);
	}

	/*
	 * Switch to the table owner's userid, so that any index functions are run
	 * as that user.  Also, lockdown security-restricted operations and
	 * arrange to make GUC variable changes local to this command.
	 *
	 * Need to do it after determining the namespace in the
	 * createPartitionTable() call.
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(splitRel->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();

	/* Copy data from the split partition to the new partitions. */
	SplitPartitionMoveRows(wqueue, rel, splitRel, cmd->partlist, newPartRels);
	/* Keep the lock until commit. */
	table_close(splitRel, NoLock);

	/* Attach new partitions to the partitioned table. */
	forboth(listptr, cmd->partlist, listptr2, newPartRels)
	{
		SinglePartitionSpec *sps = (SinglePartitionSpec *) lfirst(listptr);
		Relation	newPartRel = (Relation) lfirst(listptr2);

		/*
		 * wqueue = NULL: verification for each cloned constraint is not
		 * needed.
		 */
		attachPartitionTable(NULL, rel, newPartRel, sps->bound);
		/* Keep the lock until commit. */
		table_close(newPartRel, NoLock);
	}

	/* Drop the split partition. */
	object.classId = RelationRelationId;
	object.objectId = splitRelOid;
	object.objectSubId = 0;
	/* Probably DROP_CASCADE is not needed. */
	performDeletion(&object, DROP_RESTRICT, 0);

	/* Roll back any GUC changes executed by index functions. */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore the userid and security context. */
	SetUserIdAndSecContext(save_userid, save_sec_context);
}
