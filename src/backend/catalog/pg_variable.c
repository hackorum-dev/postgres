/*-------------------------------------------------------------------------
 *
 * pg_variable.c
 *		session variables
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/backend/catalog/pg_variable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_variable.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/syscache.h"

/*
 * Creates entry in pg_variable table
 */
ObjectAddress
create_variable(const char *varName,
				Oid varNamespace,
				Oid varType,
				int32 varTypmod,
				Oid varOwner,
				Oid varCollation,
				bool if_not_exists)
{
	NameData	varname;
	bool		nulls[Natts_pg_variable];
	Datum		values[Natts_pg_variable];
	Relation	rel;
	HeapTuple	tup;
	TupleDesc	tupdesc;
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs;
	Oid			varid;

	Assert(varName);
	Assert(OidIsValid(varNamespace));
	Assert(OidIsValid(varType));
	Assert(OidIsValid(varOwner));

	rel = table_open(VariableRelationId, RowExclusiveLock);

	/*
	 * Check for duplicates. Note that this does not really prevent
	 * duplicates, it's here just to provide nicer error message in common
	 * case. The real protection is the unique key on the catalog.
	 */
	if (SearchSysCacheExists2(VARIABLENAMENSP,
							  PointerGetDatum(varName),
							  ObjectIdGetDatum(varNamespace)))
	{
		if (if_not_exists)
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("session variable \"%s\" already exists, skipping",
							varName)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("session variable \"%s\" already exists",
							varName)));

		table_close(rel, RowExclusiveLock);

		return InvalidObjectAddress;
	}

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	namestrcpy(&varname, varName);

	varid = GetNewOidWithIndex(rel, VariableOidIndexId, Anum_pg_variable_oid);

	values[Anum_pg_variable_oid - 1] = ObjectIdGetDatum(varid);
	values[Anum_pg_variable_varcreate_lsn - 1] = LSNGetDatum(GetXLogInsertRecPtr());
	values[Anum_pg_variable_varname - 1] = NameGetDatum(&varname);
	values[Anum_pg_variable_varnamespace - 1] = ObjectIdGetDatum(varNamespace);
	values[Anum_pg_variable_vartype - 1] = ObjectIdGetDatum(varType);
	values[Anum_pg_variable_vartypmod - 1] = Int32GetDatum(varTypmod);
	values[Anum_pg_variable_varowner - 1] = ObjectIdGetDatum(varOwner);
	values[Anum_pg_variable_varcollation - 1] = ObjectIdGetDatum(varCollation);

	nulls[Anum_pg_variable_varacl - 1] = true;

	tupdesc = RelationGetDescr(rel);

	tup = heap_form_tuple(tupdesc, values, nulls);
	CatalogTupleInsert(rel, tup);
	Assert(OidIsValid(varid));

	addrs = new_object_addresses();

	ObjectAddressSet(myself, VariableRelationId, varid);

	/* dependency on namespace */
	ObjectAddressSet(referenced, NamespaceRelationId, varNamespace);
	add_exact_object_address(&referenced, addrs);

	/* dependency on used type */
	ObjectAddressSet(referenced, TypeRelationId, varType);
	add_exact_object_address(&referenced, addrs);

	/* dependency on collation */
	if (OidIsValid(varCollation) &&
		varCollation != DEFAULT_COLLATION_OID)
	{
		ObjectAddressSet(referenced, CollationRelationId, varCollation);
		add_exact_object_address(&referenced, addrs);
	}

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
	free_object_addresses(addrs);

	/* dependency on owner */
	recordDependencyOnOwner(VariableRelationId, varid, varOwner);

	/* dependency on extension */
	recordDependencyOnCurrentExtension(&myself, false);

	heap_freetuple(tup);

	/* post creation hook for new function */
	InvokeObjectPostCreateHook(VariableRelationId, varid, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}

/*
 * Drop variable by OID
 */
void
DropVariableById(Oid varid)
{
	Relation	rel;
	HeapTuple	tup;

	rel = table_open(VariableRelationId, RowExclusiveLock);

	tup = SearchSysCache1(VARIABLEOID, ObjectIdGetDatum(varid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for variable %u", varid);

	CatalogTupleDelete(rel, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(rel, RowExclusiveLock);
}
