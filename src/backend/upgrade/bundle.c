/*-------------------------------------------------------------------------
 *
 * bundle.c
 *		Rollback bookkeeping for an in-place major version upgrade
 *
 * Until an in-place upgrade goes live, it must be possible to roll it back
 * and leave the data directory exactly as the old major version shut it
 * down.  Everything a rollback needs is kept in INPLACE_UPGRADE_DIR, inside
 * the data directory, under the names in upgrade/bundle.h.  Together these
 * files are the rollback bundle:
 *
 * - copies of the files that the upgrade replaces: the control file and the
 *   statistics file, saved by the takeover at startup (see xlogupgrade.c),
 *   then the relation map files, saved before the migration of the catalogs
 *   writes anything;
 *
 * - the list of databases, with the path of each, so that a rollback can
 *   put the relation map files back without reading a catalog;
 *
 * - the list of created files.  A file that the upgrade creates is
 *   registered there, durably, before it exists, so a rollback removes
 *   exactly those files and does not have to know what they are;
 *
 * - markers, empty files that each record a completed step of the
 *   migration.
 *
 * Next to the bundle, the in-place upgrade migrator leaves the error that
 * ended its last attempt, if any, in INPLACE_UPGRADE_MIGRATE_ERROR (see
 * migrate.c).  It is not needed for a rollback.
 *
 * Nothing here is WAL-logged: these files describe the old cluster, and a
 * standby has no use for them.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/upgrade/bundle.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "access/xlog.h"
#include "common/inplace_upgrade_posture.h"
#include "miscadmin.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "upgrade/inplace_upgrade.h"

static void fsync_and_close(FILE *fp, const char *path);
static void report_unreadable_posture(const InplaceUpgradePosture *posture);


/*
 * Keep a copy of a file that the upgrade is about to replace, at the given
 * path in the bundle.  The caller has not yet changed the original, so a
 * repeated call after an error copies it again, harmlessly.
 */
void
InplaceUpgradeStashFile(const char *path, const char *stashpath)
{
	if (unlink(stashpath) < 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", stashpath)));
	copy_file(path, stashpath);
	fsync_fname(stashpath, false);
	fsync_fname(INPLACE_UPGRADE_DIR, true);
}

/*
 * Record the databases whose catalogs are migrated: one line per database,
 * holding its OID and its path relative to the data directory.
 */
void
InplaceUpgradeWriteDatabaseList(const Oid *dboids, char *const *dbpaths,
								int ndbs)
{
	FILE	   *fp;

	fp = AllocateFile(INPLACE_UPGRADE_DATABASES, "w");
	if (fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						INPLACE_UPGRADE_DATABASES)));
	for (int i = 0; i < ndbs; i++)
		fprintf(fp, "%u %s\n", dboids[i], dbpaths[i]);
	fsync_and_close(fp, INPLACE_UPGRADE_DATABASES);
}

/*
 * Flush a file written with stdio to disk, and close it.
 */
static void
fsync_and_close(FILE *fp, const char *path)
{
	if (ferror(fp) || fflush(fp) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", path)));
	if (pg_fsync(fileno(fp)) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	if (FreeFile(fp) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
	fsync_fname(INPLACE_UPGRADE_DIR, true);
}

/*
 * Add a file to the list of created files.  The path is relative to the
 * data directory.
 *
 * NB: Call this before creating the file.  A file that exists and is not on
 * the list survives a rollback.
 *
 * The first call creates the list; its caller then fsyncs INPLACE_UPGRADE_DIR.
 *
 * This must not allocate memory: callers may be inside a critical section.
 */
void
InplaceUpgradeRegisterCreatedFile(const char *path)
{
	int			fd;
	char		line[MAXPGPATH + 1];
	int			len;

	len = snprintf(line, sizeof(line), "%s\n", path);
	if (len >= sizeof(line))
		elog(ERROR, "path \"%s\" is too long to register as a created file",
			 path);

	fd = BasicOpenFile(INPLACE_UPGRADE_CREATED_FILES,
					   O_WRONLY | O_CREAT | O_APPEND | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						INPLACE_UPGRADE_CREATED_FILES)));

	errno = 0;
	if (write(fd, line, len) != len)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						INPLACE_UPGRADE_CREATED_FILES)));
	}
	if (pg_fsync(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						INPLACE_UPGRADE_CREATED_FILES)));
	if (close(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						INPLACE_UPGRADE_CREATED_FILES)));
}

/*
 * Write a marker, durably.
 */
void
InplaceUpgradeWriteMarker(const char *name)
{
	char		path[MAXPGPATH];
	int			fd;

	if (MakePGDirectory(INPLACE_UPGRADE_MARKER_DIR) < 0 && errno != EEXIST)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m",
						INPLACE_UPGRADE_MARKER_DIR)));
	fsync_fname(INPLACE_UPGRADE_DIR, true);

	snprintf(path, sizeof(path), "%s/%s", INPLACE_UPGRADE_MARKER_DIR, name);
	fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", path)));
	if (pg_fsync(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
	fsync_fname(INPLACE_UPGRADE_MARKER_DIR, true);
}

/*
 * Where this server's cluster stands in the in-place upgrade (see
 * common/inplace_upgrade_posture.c); an error if that cannot be told.
 */
void
InplaceUpgradeServerPosture(InplaceUpgradePosture *posture)
{
	GetInplaceUpgradePosture(posture);
	report_unreadable_posture(posture);
}

static void
report_unreadable_posture(const InplaceUpgradePosture *posture)
{
	if (posture->posture == UPGRADE_POSTURE_UNREADABLE)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg_internal("%s", posture->detail)));
}
