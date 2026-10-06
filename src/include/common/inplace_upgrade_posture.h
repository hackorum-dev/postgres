/*-------------------------------------------------------------------------
 *
 * inplace_upgrade_posture.h
 *		Where a data directory stands in an in-place major version upgrade
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/inplace_upgrade_posture.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COMMON_INPLACE_UPGRADE_POSTURE_H
#define COMMON_INPLACE_UPGRADE_POSTURE_H

#include "catalog/pg_control.h"

/* How far the upgrade of the cluster has got */
typedef enum UpgradePosture
{
	UPGRADE_POSTURE_UNREADABLE, /* the facts could not be read */
	UPGRADE_POSTURE_NOT_PREPARED,	/* the old version's, not at a boundary */
	UPGRADE_POSTURE_AT_BOUNDARY,	/* shut down at an upgrade boundary */
	UPGRADE_POSTURE_TAKEN_OVER, /* taken over, migration not begun */
	UPGRADE_POSTURE_MIGRATION_FAILED,	/* the migrator left an error */
	UPGRADE_POSTURE_MIGRATED,	/* catalogs migrated, not yet live */
	UPGRADE_POSTURE_LIVE,		/* the new version's for good */
} UpgradePosture;

/* The installation that runs the cluster */
typedef enum UpgradeOwner
{
	UPGRADE_OWNER_OLD,
	UPGRADE_OWNER_NEW,
} UpgradeOwner;

typedef struct InplaceUpgradePosture
{
	UpgradePosture posture;
	UpgradeOwner owner;
	char		detail[MAXPGPATH + 128];	/* why the posture is UNREADABLE */
} InplaceUpgradePosture;

extern bool InplaceUpgradeTranslateControl(ControlFileData *cf, bool *crc_ok);
extern int	InplaceUpgradeReadControlFile(const char *path, ControlFileData *cf,
										  bool *crc_ok);
extern void InplaceUpgradeGetPosture(const ControlFileData *cf,
									 const char *datadir,
									 InplaceUpgradePosture *result);

#endif							/* COMMON_INPLACE_UPGRADE_POSTURE_H */
