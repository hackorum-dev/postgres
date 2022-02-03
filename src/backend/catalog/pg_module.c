/*-------------------------------------------------------------------------
 *
 * pg_module.c
 *	  routines to support manipulation of the pg_module relation
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_module.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_module.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/* ----------------
 * ModuleCreate
 *
 * Create a module with the given name and owner OID.
 *
 * ---------------
 */
Oid
ModuleCreate(const char *modName, const char *nspName, Oid ownerId)
{
	Relation	modDesc;
	HeapTuple	tup;
	Oid     	modOid;
	Oid     	nspOid;
	bool		nulls[Natts_pg_module];
	Datum		values[Natts_pg_module];
	NameData	nname;
	TupleDesc	tupDesc;
	ObjectAddress myself;
	int     	i;
	Acl        *modAcl;

	/* sanity checks */
	if (!modName)
		elog(ERROR, "no module name supplied");

	if (!nspName)
		elog(ERROR, "no parent namespace name supplied");

	nspOid = get_namespace_oid(nspName, false);

	if (SearchSysCacheExists2(MODULENAME, PointerGetDatum(modName), ObjectIdGetDatum(nspOid)))
		ereport(ERROR, (errcode(ERRCODE_DUPLICATE_MODULE),
						errmsg("module \"%s\" already exists in schema \"%s\"", modName, nspName)));

	modAcl = get_user_default_acl(OBJECT_MODULE, ownerId,
								  InvalidOid);

	modDesc = table_open(ModuleRelationId, RowExclusiveLock);
	tupDesc = modDesc->rd_att;

	/* initialize nulls and values */
	for (i = 0; i < Natts_pg_module; i++)
	{
		nulls[i] = false;
		values[i] = (Datum) NULL;
	}

	modOid = GetNewOidWithIndex(modDesc, ModuleOidIndexId,
								Anum_pg_module_oid);
	values[Anum_pg_module_oid - 1] = ObjectIdGetDatum(modOid);

	namestrcpy(&nname, modName);
	//namestrcpy(&schemaname, nspName);

	values[Anum_pg_module_modname - 1] = NameGetDatum(&nname);
	values[Anum_pg_module_nspoid - 1] = ObjectIdGetDatum(nspOid);
	values[Anum_pg_module_modowner - 1] = ObjectIdGetDatum(ownerId);
	if (modAcl != NULL)
		values[Anum_pg_module_modacl - 1] = PointerGetDatum(modAcl);
	else
		nulls[Anum_pg_module_modacl - 1] = true;

	tup = heap_form_tuple(tupDesc, values, nulls);

	CatalogTupleInsert(modDesc, tup);
	Assert(OidIsValid(modOid));

	table_close(modDesc, RowExclusiveLock);

	/* Record dependencies */
	myself.classId = ModuleRelationId;
	myself.objectId = modOid;
	myself.objectSubId = 0;

	/* dependency on owner */
	recordDependencyOnOwner(ModuleRelationId, modOid, ownerId);

	/* dependencies on roles mentioned in default ACL */
	recordDependencyOnNewAcl(ModuleRelationId, modOid, 0, ownerId, modAcl);

	/* dependency on extension */
	recordDependencyOnCurrentExtension(&myself, false);

	/* Post creation hook for new schema */
	InvokeObjectPostCreateHook(ModuleRelationId, modOid, 0);

	return modOid;
}
