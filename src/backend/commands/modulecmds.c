/*-------------------------------------------------------------------------
 *
 * modulecmds.c
 *	  module creation/manipulation commands
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/modulecmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_module.h"
#include "catalog/pg_namespace.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/modulecmds.h"
#include "miscadmin.h"
#include "parser/parse_utilcmd.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

static void AlterModuleOwner_internal(HeapTuple tup, Relation rel, Oid newOwnerId);

/*
 * CREATE MODULE
 *
 * Note: caller should pass in location information for the whole
 * CREATE MODULE statement, which in turn we pass down as the location
 * of the component commands.  This comports with our general plan of
 * reporting location/len for the whole command even when executing
 * a subquery.
 */
ObjectAddress
CreateModuleCommand(CreateModuleStmt *stmt, const char *queryString,
					int stmt_location, int stmt_len)
{
	char	   *modulename;
	char	   *schemaname;
	Oid		    moduleId;
	Oid		    namespaceId;
	OverrideSearchPath *overridePath;
	List	   *parsetree_list;
	ListCell   *parsetree_item;
	Oid		    owner_uid;
	Oid		    saved_uid;
	int		    save_sec_context;
	AclResult	aclresult;
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs;
	Oid 	    dummyOid;

	GetUserIdAndSecContext(&saved_uid, &save_sec_context);

	if (stmt->authrole)
		owner_uid = get_rolespec_oid(stmt->authrole, false);
	else
		owner_uid = saved_uid;

	/* Convert list of names to a name and namespace */
	namespaceId = QualifiedNameWithModuleGetCreationNamespace(stmt->modulename,
															  &dummyOid, &modulename);
	schemaname = get_namespace_name(namespaceId);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA, schemaname);

	/*
	 * If if_not_exists was given and the module already exists, bail out.
	 * (Note: we needn't check this when not if_not_exists, because
	 * ModuleCreate will complain anyway.)  We could do this before making
	 * the permissions checks, but since CREATE TABLE IF NOT EXISTS makes its
	 * creation-permission check first, we do likewise.
	 */

	if (stmt->if_not_exists &&
		SearchSysCacheExists2(MODULENAME, PointerGetDatum(modulename),
							  ObjectIdGetDatum(namespaceId)))
	{
		ereport(NOTICE,
				(errcode(ERRCODE_DUPLICATE_MODULE),
				 errmsg("module \"%s\" already exists in schema \"%s\", skipping",
						modulename, schemaname)));
		return InvalidObjectAddress;
	}

	/*
	 * If the requested authorization is different from the current user,
	 * temporarily set the current user so that the object(s) will be created
	 * with the correct ownership.
	 *
	 * (The setting will be restored at the end of this routine, or in case of
	 * error, transaction abort will clean things up.)
	 */

	if (saved_uid != owner_uid)
		SetUserIdAndSecContext(owner_uid,
							   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);

	/* Create the module's entry in catalog in pg_module */
	moduleId = ModuleCreate(modulename, schemaname, owner_uid);

	/* Advance cmd counter to make the namespace visible */
	CommandCounterIncrement();

	/*
	 * Temporarily make the new namespace be the front of the search path, as
	 * well as the default creation target namespace.  This will be undone at
	 * the end of this routine, or upon error.
	 */
	overridePath = GetOverrideSearchPath(CurrentMemoryContext);
	overridePath->schemas = lcons_oid(moduleId, overridePath->schemas);
	/* TODO should we clear overridePath->useTemp? */
	PushOverrideSearchPath(overridePath);

	/*
	 * Report the new module to possibly interested event triggers.  Note we
	 * must do this here and not in ProcessUtilitySlow because otherwise the
	 * objects created below are reported before the module, which would be
	 * wrong.
	 */
	ObjectAddressSet(myself, ModuleRelationId, moduleId);
	EventTriggerCollectSimpleCommand(myself, InvalidObjectAddress, (Node *) stmt);

	/*
	 * Examine the list of commands embedded in the CREATE MODULE command, and
	 * reorganize them into a sequentially executable order with no forward
	 * references.  Note that the result is still a list of raw parsetrees ---
	 * we cannot, in general, run parse analysis on one statement until we
	 * have actually executed the prior ones.
	 */
	parsetree_list = transformCreateModuleStmt(stmt);

	/*
	 * Execute each command contained in the CREATE MODULE.  Since the grammar
	 * allows only utility commands in CREATE MODULE, there is no need to pass
	 * them through parse_analyze() or the rewriter; we can just hand them
	 * straight to ProcessUtility.
	 */
	foreach(parsetree_item, parsetree_list)
	{
		Node	   *stmt = (Node *) lfirst(parsetree_item);
		PlannedStmt *wrapper;

		wrapper = makeNode(PlannedStmt);
		wrapper->commandType = CMD_UTILITY;
		wrapper->canSetTag = false;
		wrapper->utilityStmt = stmt;
		wrapper->stmt_location = stmt_location;
		wrapper->stmt_len = stmt_len;

		ProcessUtilityUsingModule(wrapper,
								  queryString,
								  false,
								  PROCESS_UTILITY_SUBCOMMAND,
								  NULL,
								  NULL,
								  None_Receiver,
								  NULL,
								  namespaceId,
								  moduleId);

		CommandCounterIncrement();
	}

	/* Reset search path to normal state */
	PopOverrideSearchPath();

	/* Reset current user and security context */
	SetUserIdAndSecContext(saved_uid, save_sec_context);

	addrs = new_object_addresses();

	/* dependency on namespace */
	ObjectAddressSet(referenced, ModuleRelationId, namespaceId);
	add_exact_object_address(&referenced, addrs);

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
	free_object_addresses(addrs);

	recordDependencyOnOwner(ModuleRelationId, moduleId, owner_uid);

	return myself;
}

/*
 * Alter module ... create/replace function
 */
ObjectAddress
AlterModuleCreateReplaceFunction(AlterModuleCreateReplaceFuncStmt *stmt, const char *queryString,
								 int stmt_location, int stmt_len)
{
	Oid namespaceOid;
	Oid moduleOid;
	Oid dummyOid;
	char *moduleName;
	ObjectAddress address;
	Relation rel;
	HeapTuple tup;
	CreateFunctionStmt *fstmt;
	Node *element = stmt->createreplacefunction;
	Form_pg_module modForm;
	AclResult	aclresult;

	rel = table_open(ModuleRelationId, RowExclusiveLock);

	/* Convert qualified names List to a name and namespace */
	namespaceOid = QualifiedNameWithModuleGetCreationNamespace
		(stmt->modulename, &dummyOid, &moduleName);

	/* Permission check: must have create permission on namespace */
	aclresult = pg_namespace_aclcheck(namespaceOid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA, get_namespace_name(namespaceOid));

	/* Permission check: must have create permission on module */
	moduleOid = get_module_oid_from_name(namespaceOid, NameListToString(stmt->modulename), true);
	aclresult = pg_module_aclcheck(moduleOid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_MODULE, NameListToString(stmt->modulename));

	tup = SearchSysCacheCopy2(MODULENAME, PointerGetDatum(moduleName),
							  ObjectIdGetDatum(namespaceOid));
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for module %s", NameListToString(stmt->modulename));

	modForm = (Form_pg_module) GETSTRUCT(tup);
	moduleOid = modForm->oid;

	ObjectAddressSet(address, ModuleRelationId, moduleOid);

	switch (nodeTag(element))
	{
		case T_CreateFunctionStmt:
		{
			fstmt = (CreateFunctionStmt *) element;

			if (list_length(fstmt->funcname) > 1)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_MODULE_DEFINITION),
						 errmsg("CREATE/REPLACE FUNCTION (%s) specifies a "
								"namespace inside of ALTER MODULE (%s)",
								NameListToString(fstmt->funcname),
								NameListToString(stmt->modulename))));

			break;
		}

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(element));
	}

	PlannedStmt *wrapper;

	wrapper = makeNode(PlannedStmt);
	wrapper->commandType = CMD_UTILITY;
	wrapper->canSetTag = false;
	wrapper->utilityStmt = element;
	wrapper->stmt_location = stmt_location;
	wrapper->stmt_len = stmt_len;

	ProcessUtilityUsingModule(wrapper,
							  queryString,
							  false,
							  PROCESS_UTILITY_SUBCOMMAND,
							  NULL,
							  NULL,
							  None_Receiver,
							  NULL,
							  namespaceOid,
							  moduleOid);

	CommandCounterIncrement();

	table_close(rel, NoLock);
	heap_freetuple(tup);

	return address;
}

/*
 * Implements the ALTER MODULE utility command (except for the
 * RENAME and OWNER clauses, which are handled as part of the generic
 * ALTER framework).
 */
ObjectAddress
AlterModuleAlterFunction(ParseState *pstate, AlterModuleAlterFuncStmt *stmt)
{
	char	   *moduleName;
	Oid			namespaceId;
	Oid			moduleOid;
	Oid			dummyOid;
	Relation	rel;
	HeapTuple	tup;
	ObjectAddress address;
	AlterFunctionStmt *alterfuncstmt;
	Form_pg_module modForm;

	alterfuncstmt = stmt->alterfuncstmt;

	// TODO Do we need a lock? To prevent concurrent updates creating an inconsistent state for the module
	rel = table_open(ModuleRelationId, RowExclusiveLock);

	/* Convert qualified names List to a name and namespace */
	namespaceId = QualifiedNameWithModuleGetCreationNamespace(stmt->modulename,
															  &dummyOid, &moduleName);
	tup = SearchSysCacheCopy2(MODULENAME, PointerGetDatum(moduleName),
							  ObjectIdGetDatum(namespaceId));
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for module %s", NameListToString(stmt->modulename));

	modForm = (Form_pg_module) GETSTRUCT(tup);
	moduleOid = modForm->oid;

	/* Permission check: must have create permission on module */
	if (!pg_module_ownercheck(moduleOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, alterfuncstmt->objtype,
					   NameListToString(alterfuncstmt->func->objname));

	AlterFunction(pstate, alterfuncstmt);

	CommandCounterIncrement();

	table_close(rel, NoLock);
	heap_freetuple(tup);

	return address;
}

/*
 * Rename module
 */
ObjectAddress
AlterModuleRename(AlterModuleRenameStmt *stmt)
{
	Oid			namespaceId;
	Oid			modOid;
	Oid			dummyOid;
	List	   *oldName;
	char	   *newName;
	char	   *moduleName;
	HeapTuple	tup;
	Relation	rel;
	ObjectAddress address;
	Form_pg_module modForm;

	oldName = stmt->modulename;
	newName = stmt->newname;

	/* Convert qualified names List to a name and namespace */
	namespaceId = QualifiedNameWithModuleGetCreationNamespace(oldName, &dummyOid, &moduleName);

	rel = table_open(ModuleRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy2(MODULENAME, PointerGetDatum(newName),
							  PointerGetDatum(namespaceId));
	if (HeapTupleIsValid(tup)) /* can't rename to a module name that already exists */
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_MODULE),
				 errmsg("module \"%s\" already exists in schema \"%d\"", newName, namespaceId)));

	tup = SearchSysCacheCopy2(MODULENAME, PointerGetDatum(moduleName),
							  ObjectIdGetDatum(namespaceId));
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "module \"%s\" does not exist", NameListToString(oldName));

	modForm = (Form_pg_module) GETSTRUCT(tup);
	modOid = modForm->oid;

	/* must be owner */
	if (!superuser() && !pg_module_ownercheck(modOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_MODULE, moduleName);

	/* rename */
	namestrcpy(&modForm->modname, newName);

	CatalogTupleUpdate(rel, &tup->t_self, tup);

	InvokeObjectPostAlterHook(ModuleRelationId, modForm->oid, 0);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	ObjectAddressSet(address, ModuleRelationId, modOid);

	return address;
}

/*
 * Change module owner
 */
ObjectAddress
AlterModuleOwner(AlterModuleOwnerStmt *stmt)
{
	Oid			namespaceId;
	Oid			modOid;
	Oid			dummyOid;
	List	   *name;
	Oid			newOwnerId;
	char	   *moduleName;

	HeapTuple	tup;
	Relation	rel;
	ObjectAddress address;
	Form_pg_module modForm;

	name = stmt->modulename;
	newOwnerId = get_rolespec_oid(stmt->newowner, false);

	/* Convert qualified names List to a name and namespace */
	namespaceId = QualifiedNameWithModuleGetCreationNamespace(name, &dummyOid, &moduleName);

	rel = table_open(ModuleRelationId, RowExclusiveLock);
	tup = SearchSysCacheCopy2(MODULENAME, PointerGetDatum(name), ObjectIdGetDatum(namespaceId));
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "module %s does not exist", NameListToString(name));

	modForm = (Form_pg_module) GETSTRUCT(tup);
	modOid = modForm->oid;

	AlterModuleOwner_internal(tup, rel, newOwnerId);

	ObjectAddressSet(address, ModuleRelationId, modOid);

	ReleaseSysCache(tup);

	table_close(rel, RowExclusiveLock);

	return address;
}

static void
AlterModuleOwner_internal(HeapTuple tup, Relation rel, Oid newOwnerId)
{
	Form_pg_module modForm;
	modForm = (Form_pg_module) GETSTRUCT(tup);

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is for dump restoration purposes.
	 */
	if (modForm->modowner != newOwnerId)
	{
		Datum		repl_val[Natts_pg_module];
		bool		repl_null[Natts_pg_module];
		bool		repl_repl[Natts_pg_module];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;

		/* Superusers can always do it. Otherwise, must be owner of the existing object */
		if (!superuser() && !pg_module_ownercheck(modForm->oid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_MODULE,
						   NameStr(modForm->modname));

		/* Must be able to become new owner */
		check_is_member_of_role(GetUserId(), newOwnerId);

		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		repl_repl[Anum_pg_module_modowner - 1] = true;
		repl_val[Anum_pg_module_modowner - 1] = ObjectIdGetDatum(newOwnerId);

		/*
		 * Determine the modified ACL for the new owner.  This is only
		 * necessary when the ACL is non-null.
		 */
		aclDatum = SysCacheGetAttr(MODULENAME, tup, Anum_pg_module_modacl, &isNull);
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 modForm->modowner, newOwnerId);
			repl_repl[Anum_pg_module_modacl - 1] = true;
			repl_val[Anum_pg_module_modacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modify_tuple(tup, RelationGetDescr(rel), repl_val, repl_null, repl_repl);

		heap_freetuple(newtuple);

		/* Update owner dependency reference */
		changeDependencyOnOwner(ModuleRelationId, modForm->oid,
								newOwnerId);
	}

	InvokeObjectPostAlterHook(ModuleRelationId,
							  modForm->oid, 0);
}
