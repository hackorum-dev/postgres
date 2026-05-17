/*-------------------------------------------------------------------------
 *
 * keyjoincmds.c
 *	  Commands for manipulating stored key-join proofs.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/keyjoincmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/skey.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_rewrite.h"
#include "common/int.h"
#include "commands/keyjoin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parse_key_join.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/injection_point.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static Oid	get_rule_event_relation(Oid ruleOid);
static List *find_dependent_key_join_objects(Oid refclassid, Oid refobjid);
static void revalidate_dependent_key_join_objects(List *objects);
static void revalidate_dependent_key_join_object(const ObjectAddress *object);
static bool object_address_list_member(List *objects, Oid classId,
									   Oid objectId);
static int	object_address_list_cmp(const ListCell *a, const ListCell *b);
static ObjectAddress *make_object_address(Oid classId, Oid objectId);
static void revalidate_dependent_key_join_relation(Oid relationOid);
static void revalidate_dependent_key_join_function(Oid procOid);
static void revalidate_dependent_key_join_policy(Oid policy_id);
static void revalidate_stored_key_join_node(Node *stored);

static Oid
get_rule_event_relation(Oid ruleOid)
{
	Relation	rewriteRel;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			result = InvalidOid;

	rewriteRel = table_open(RewriteRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_rewrite_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ruleOid));
	scan = systable_beginscan(rewriteRel, RewriteOidIndexId, true,
							  NULL, 1, key);
	tup = systable_getnext(scan);
	if (!HeapTupleIsValid(tup))
	{
		/*
		 * The dependent rule can be dropped after pg_depend is scanned but
		 * before this lookup acquires pg_rewrite.  Then the owning object is
		 * gone too, so there is no stored proof left to revalidate.
		 */
		systable_endscan(scan);
		table_close(rewriteRel, AccessShareLock);
		return InvalidOid;
	}
	result = ((Form_pg_rewrite) GETSTRUCT(tup))->ev_class;
	Assert(OidIsValid(result));
	systable_endscan(scan);
	table_close(rewriteRel, AccessShareLock);

	return result;
}

static List *
find_dependent_key_join_objects(Oid refclassid, Oid refobjid)
{
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *result = NIL;

	depRel = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(refclassid));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(refobjid));
	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tup);
		Oid			classid;
		Oid			objectid;

		if (dep->deptype != DEPENDENCY_KEYJOIN)
			continue;

		if (dep->classid == RewriteRelationId)
		{
			classid = RelationRelationId;
			objectid = get_rule_event_relation(dep->objid);
			if (!OidIsValid(objectid))
				continue;
		}
		else if (dep->classid == ProcedureRelationId)
		{
			classid = ProcedureRelationId;
			objectid = dep->objid;
		}
		else
		{
			Assert(dep->classid == PolicyRelationId);
			classid = PolicyRelationId;
			objectid = dep->objid;
		}

		if (classid == refclassid && objectid == refobjid)
			continue;

		if (!object_address_list_member(result, classid, objectid))
			result = lappend(result, make_object_address(classid, objectid));
	}

	systable_endscan(scan);
	table_close(depRel, AccessShareLock);

	list_sort(result, object_address_list_cmp);
	return result;
}

static void
revalidate_dependent_key_join_objects(List *objects)
{
	foreach_ptr(ObjectAddress, object, objects)
		revalidate_dependent_key_join_object(object);
}

static void
revalidate_dependent_key_join_object(const ObjectAddress *object)
{
	switch (object->classId)
	{
		case RelationRelationId:
			revalidate_dependent_key_join_relation(object->objectId);
			break;
		case ProcedureRelationId:
			revalidate_dependent_key_join_function(object->objectId);
			break;
		default:
			Assert(object->classId == PolicyRelationId);
			revalidate_dependent_key_join_policy(object->objectId);
			break;
	}
}

static void
revalidate_dependent_key_join_relation(Oid relationOid)
{
	Relation	rel;

	rel = relation_open(relationOid, AccessShareLock);

	/*
	 * Walk every rule attached to the dependent relation.  A stored key-join
	 * proof can live in any rule action: a view's or matview's _RETURN rule,
	 * an INSTEAD-OF rule on a view, a DO ALSO/INSTEAD rule on a plain table,
	 * etc.  Each key-join-bearing action must be revalidated so DDL that
	 * would make the proof unprovable is rejected with the existing "key join
	 * cannot be proven from available constraints" error.
	 *
	 * Revalidation may change copied KeyJoinNode dependency lists.
	 * Dependency shrinkage is safe to leave stale in pg_depend, but new
	 * dependencies or other semantic changes would make the stored proof
	 * unsafe without rewriting its owning object.
	 */
	if (rel->rd_rules == NULL)
	{
		/*
		 * A rule can be dropped after pg_depend is scanned but before this
		 * relation lock is acquired.  RelationBuildRuleLock() leaves
		 * rd_rules NULL when no rules remain, so there is no stored proof
		 * left on this relation to revalidate.
		 */
		relation_close(rel, AccessShareLock);
		return;
	}
	Assert(rel->rd_rules->numLocks > 0);
	for (int i = 0; i < rel->rd_rules->numLocks; i++)
	{
		RewriteRule *rule = rel->rd_rules->rules[i];

		foreach_node(Query, action, rule->actions)
			revalidate_stored_key_join_node((Node *) action);

		if (rule->qual != NULL)
			revalidate_stored_key_join_node(rule->qual);
	}

	relation_close(rel, NoLock);
}

static void
revalidate_dependent_key_join_function(Oid procOid)
{
	HeapTuple	tup;
	Datum		datum;
	bool		isnull;
	Node	   *body;

	INJECTION_POINT("key-join-before-dependent-function-lookup", NULL);

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(procOid));
	if (!HeapTupleIsValid(tup))
	{
		/*
		 * A function can be dropped after pg_depend is scanned but before
		 * this lookup reaches pg_proc.  Its stored proof no longer exists.
		 */
		return;
	}

	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_prosqlbody, &isnull);
	if (isnull)
	{
		ReleaseSysCache(tup);
		return;
	}

	body = stringToNode(TextDatumGetCString(datum));
	ReleaseSysCache(tup);

	revalidate_stored_key_join_node(body);
}

static void
revalidate_dependent_key_join_policy(Oid policy_id)
{
	Relation	pg_policy_rel;
	ScanKeyData skey[1];
	SysScanDesc sscan;
	HeapTuple	policy_tuple;
	TupleDesc	policy_desc;
	Datum		expr_datum;
	bool		expr_isnull;

	pg_policy_rel = table_open(PolicyRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_policy_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(policy_id));

	sscan = systable_beginscan(pg_policy_rel, PolicyOidIndexId, true, NULL,
							   1, skey);
	policy_tuple = systable_getnext(sscan);

	if (!HeapTupleIsValid(policy_tuple))
	{
		/*
		 * A policy can be dropped after pg_depend is scanned but before this
		 * lookup acquires pg_policy.  In that case its stored proof is gone.
		 */
		systable_endscan(sscan);
		table_close(pg_policy_rel, AccessShareLock);
		return;
	}

	policy_desc = RelationGetDescr(pg_policy_rel);

	expr_datum = heap_getattr(policy_tuple, Anum_pg_policy_polqual,
							  policy_desc, &expr_isnull);
	if (!expr_isnull)
		revalidate_stored_key_join_node(
			stringToNode(TextDatumGetCString(expr_datum)));

	expr_datum = heap_getattr(policy_tuple, Anum_pg_policy_polwithcheck,
							  policy_desc, &expr_isnull);
	if (!expr_isnull)
		revalidate_stored_key_join_node(
			stringToNode(TextDatumGetCString(expr_datum)));

	systable_endscan(sscan);
	table_close(pg_policy_rel, AccessShareLock);
}

static void
revalidate_stored_key_join_node(Node *stored)
{
	Node	   *copy;

	Assert(stored != NULL);

	if (!storedNodeContainsKeyJoin(stored))
		return;

	copy = copyObject(stored);
	revalidateStoredKeyJoinProofsInNode(copy);

	if (!revalidatedStoredKeyJoinProofsAreSafe(stored, copy))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("stored key join proof would require new dependencies")));
}

void
RevalidateDependentKeyJoinObjectsOnConstraint(Oid constraintOid)
{
	List	   *objects;

	Assert(OidIsValid(constraintOid));

	objects = find_dependent_key_join_objects(ConstraintRelationId,
											  constraintOid);
	revalidate_dependent_key_join_objects(objects);
}

void
RevalidateDependentKeyJoinObjectsOnRelation(Oid relationOid)
{
	List	   *objects;

	Assert(OidIsValid(relationOid));

	objects = find_dependent_key_join_objects(RelationRelationId,
											  relationOid);
	revalidate_dependent_key_join_objects(objects);
}

void
RevalidateDependentKeyJoinObjectsOnProcedure(Oid procOid)
{
	List	   *objects;

	Assert(OidIsValid(procOid));

	/*
	 * CREATE OR REPLACE FUNCTION transforms a new-style SQL function body
	 * before the replacement tuple is visible.  If that body proves a key
	 * join through objects that refer back to the function, it may have used
	 * the old function properties; validate the function's own body once
	 * against the final catalog state before checking direct dependents.
	 */
	revalidate_dependent_key_join_function(procOid);
	objects = find_dependent_key_join_objects(ProcedureRelationId, procOid);
	revalidate_dependent_key_join_objects(objects);
}

static bool
object_address_list_member(List *objects, Oid classId, Oid objectId)
{
	foreach_ptr(ObjectAddress, object, objects)
	{
		Assert(object->objectSubId == 0);

		if (object->classId == classId && object->objectId == objectId)
			return true;
	}
	return false;
}

static int
object_address_list_cmp(const ListCell *a, const ListCell *b)
{
	const ObjectAddress *obj1 = (const ObjectAddress *) lfirst(a);
	const ObjectAddress *obj2 = (const ObjectAddress *) lfirst(b);
	int			cmp;

	Assert(obj1->objectSubId == 0);
	Assert(obj2->objectSubId == 0);

	cmp = pg_cmp_u32(obj1->classId, obj2->classId);
	if (cmp != 0)
		return cmp;
	return pg_cmp_u32(obj1->objectId, obj2->objectId);
}

static ObjectAddress *
make_object_address(Oid classId, Oid objectId)
{
	ObjectAddress *object = palloc_object(ObjectAddress);

	ObjectAddressSet(*object, classId, objectId);
	return object;
}
