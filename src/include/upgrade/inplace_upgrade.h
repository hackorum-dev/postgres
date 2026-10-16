/*-------------------------------------------------------------------------
 *
 * inplace_upgrade.h
 *		Rollback bookkeeping for an in-place major version upgrade
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/upgrade/inplace_upgrade.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INPLACE_UPGRADE_H
#define INPLACE_UPGRADE_H

#include "access/htup.h"
#include "nodes/pg_list.h"
#include "storage/relfilelocator.h"
#include "upgrade/bundle.h"

struct CatalogRelation;			/* see catalog/catalog_relations.h */
struct InplaceUpgradePosture;	/* see common/inplace_upgrade_posture.h */

/* A database whose catalogs are migrated */
typedef struct MigrateDatabase
{
	Oid			oid;
	Oid			tablespace;
	NameData	name;
	char	   *path;			/* relative to the data directory */
} MigrateDatabase;

/* What the migration needs of a pg_class row */
typedef struct ClassEntry
{
	Oid			oid;
	RelFileNumber relfilenode;	/* 0 for a mapped relation */
} ClassEntry;

/* Callback of InplaceUpgradeScanOldCatalog(), given each tuple */
typedef void (*OldTupleCallback) (HeapTuple tuple, void *arg);

/* A relation of a catalog that the migration rewrites under a new file */
typedef struct MigrateRelation
{
	Oid			reloid;
	const char *name;
	RelFileLocator oldlocator;
	RelFileLocator newlocator;
	bool		mapped;			/* in the relation map, not in pg_class */
} MigrateRelation;

/* The new file number that the rewritten pg_class row of a relation gets */
typedef struct FilenodeFixup
{
	Oid			reloid;
	RelFileNumber relfilenode;
} FilenodeFixup;

/* in upgrade/migrate.c */
pg_noreturn extern void InplaceUpgradeMigratorMain(const void *startup_data,
												   size_t startup_data_len);

/* in upgrade/oldcatalog.c */
extern void InplaceUpgradeCheckOldMajorVersion(void);
extern void InplaceUpgradeScanOldCatalog(RelFileLocator rlocator,
										 const char *relname,
										 OldTupleCallback callback,
										 void *arg);
extern List *InplaceUpgradeGetDatabases(void);
extern ClassEntry *InplaceUpgradeReadClasses(const MigrateDatabase *db,
											 int *nclasses);
extern const ClassEntry *InplaceUpgradeLookupClass(const ClassEntry *classes,
												   int nclasses, Oid reloid);

/* in upgrade/rewrite.c */
extern void InplaceUpgradeRewriteCatalog(const struct CatalogRelation *cat,
										 const MigrateRelation *heap,
										 const MigrateRelation *indexes,
										 const MigrateRelation *toast,
										 const MigrateRelation *toastindex,
										 const FilenodeFixup *fixups,
										 int nfixups);

/* in upgrade/bundle.c */
extern void InplaceUpgradeStashFile(const char *path, const char *stashpath);
extern void InplaceUpgradeWriteDatabaseList(const Oid *dboids,
											char *const *dbpaths, int ndbs);
extern bool InplaceUpgradeReadDatabaseList(bool missing_ok, int *ndbs,
										   Oid **dboids, char ***dbpaths);
extern void InplaceUpgradeRegisterCreatedFile(const char *path);
extern void InplaceUpgradeWriteMarker(const char *name);
extern void InplaceUpgradeServerPosture(struct InplaceUpgradePosture *posture);
pg_noreturn extern void InplaceUpgradeRollbackMain(int argc, char *argv[]);

#endif							/* INPLACE_UPGRADE_H */
