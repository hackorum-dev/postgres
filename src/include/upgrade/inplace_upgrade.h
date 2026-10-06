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

struct InplaceUpgradePosture;	/* see common/inplace_upgrade_posture.h */

/* A database whose catalogs are migrated */
typedef struct MigrateDatabase
{
	Oid			oid;
	Oid			tablespace;
	NameData	name;
	char	   *path;			/* relative to the data directory */
} MigrateDatabase;

/* Callback of InplaceUpgradeScanOldCatalog(), given each tuple */
typedef void (*OldTupleCallback) (HeapTuple tuple, void *arg);

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

/* in upgrade/bundle.c */
extern void InplaceUpgradeStashFile(const char *path, const char *stashpath);
extern void InplaceUpgradeWriteDatabaseList(const Oid *dboids,
											char *const *dbpaths, int ndbs);
extern void InplaceUpgradeRegisterCreatedFile(const char *path);
extern void InplaceUpgradeWriteMarker(const char *name);
extern void InplaceUpgradeServerPosture(struct InplaceUpgradePosture *posture);

#endif							/* INPLACE_UPGRADE_H */
