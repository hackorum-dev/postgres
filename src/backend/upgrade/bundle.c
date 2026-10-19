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
 * "postgres --inplace-upgrade-rollback" (InplaceUpgradeRollbackMain, below)
 * reads the bundle to roll the upgrade back, and removes it.
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
#include "access/xlog_internal.h"
#include "access/xlogupgrade.h"
#include "catalog/pg_control.h"
#include "common/inplace_upgrade_posture.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "upgrade/inplace_upgrade.h"
#include "utils/guc.h"
#include "utils/relmapper.h"

static void fsync_and_close(FILE *fp, const char *path);
static void report_unreadable_posture(const InplaceUpgradePosture *posture);
static void read_control_file(const char *path, ControlFileData *cf);
static int	remove_created_files(void);
static void restore_relation_maps(void);
static void restore_file(const char *name, const char *path);


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
 * Read the list of databases whose catalogs are migrated, written by
 * InplaceUpgradeWriteDatabaseList(), into palloc'd arrays.  Returns false if
 * there is no list, which is an error unless missing_ok.
 */
bool
InplaceUpgradeReadDatabaseList(bool missing_ok, int *ndbs, Oid **dboids,
							   char ***dbpaths)
{
	FILE	   *fp;
	char		line[MAXPGPATH + 16];
	int			maxdbs = 16;

	fp = AllocateFile(INPLACE_UPGRADE_DATABASES, "r");
	if (fp == NULL)
	{
		if (errno == ENOENT && missing_ok)
			return false;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						INPLACE_UPGRADE_DATABASES)));
	}

	*ndbs = 0;
	*dboids = palloc_array(Oid, maxdbs);
	*dbpaths = palloc_array(char *, maxdbs);
	while (fgets(line, sizeof(line), fp) != NULL)
	{
		char	   *end;
		unsigned long dboid;

		line[strcspn(line, "\n")] = '\0';
		errno = 0;
		dboid = strtoul(line, &end, 10);
		if (errno != 0 || end == line || *end != ' ' || dboid > PG_UINT32_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid line \"%s\" in file \"%s\"",
							line, INPLACE_UPGRADE_DATABASES)));

		if (*ndbs == maxdbs)
		{
			maxdbs *= 2;
			*dboids = repalloc_array(*dboids, Oid, maxdbs);
			*dbpaths = repalloc_array(*dbpaths, char *, maxdbs);
		}
		(*dboids)[*ndbs] = (Oid) dboid;
		(*dbpaths)[*ndbs] = pstrdup(end + 1);
		(*ndbs)++;
	}
	if (ferror(fp))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m",
						INPLACE_UPGRADE_DATABASES)));
	FreeFile(fp);
	return true;
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

/*
 * postgres --inplace-upgrade-rollback -D DATADIR
 *
 * Roll back an in-place upgrade that has not gone live, with the server shut
 * down: remove every file on the list of created files, put back the copies
 * of the files the upgrade replaced, and remove the bundle.  The control
 * file goes back last: until then the data directory is the new version's,
 * and after that it is the old version's, shut down at its upgrade boundary.
 *
 * Three things stay as the new version left them, because the old version
 * recreates them anyway: the relation cache init files, which the new
 * version removed at startup and wrote again; pg_subtrans, whose segments
 * the new version's shutdown checkpoint truncated and created; and the WAL
 * segments from the start segment on, which the old version had recycled or
 * preallocated and the takeover removed (see xlogupgrade.c).  The old
 * version preallocates again as it writes.
 *
 * Removing the stashed control file is the commit point.  Until then,
 * running this again redoes every step; after it, only the removal of the
 * rest of the bundle is left to do.
 */
void
InplaceUpgradeRollbackMain(int argc, char *argv[])
{
	const char *datadir = NULL;
	ControlFileData current;
	ControlFileData stashed;
	bool		crc_ok;
	int			r;
	InplaceUpgradePosture posture;
	bool		taken_over;
	int			nremoved;

	/* argv[1] is --inplace-upgrade-rollback */
	if (argc == 4 && strcmp(argv[2], "-D") == 0)
		datadir = argv[3];
	else
	{
		write_stderr("usage: %s --inplace-upgrade-rollback -D DATADIR\n",
					 argv[0]);
		exit(1);
	}

	InitStandaloneProcess(argv[0]);

	/*
	 * WIP: the configuration files are not read, so log_timezone and
	 * log_line_prefix keep their built-in defaults, and what this writes to
	 * the server log does not match the server's own lines.
	 */
	InitializeGUCOptions();
	SetDataDir(datadir);
	checkDataDir();
	ChangeToDataDir();

	/* Fails if a server is running. */
	CreateDataDirLockFile(false);

	if (access(INPLACE_UPGRADE_DIR, F_OK) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("there is no in-place upgrade to roll back in data directory \"%s\"",
						DataDir)));

	r = InplaceUpgradeReadControlFile(XLOG_CONTROL_FILE, &current, &crc_ok);
	if (r < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", XLOG_CONTROL_FILE)));
	if (r != sizeof(ControlFileData))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("could not read file \"%s\": read %d of %zu",
						XLOG_CONTROL_FILE, r, sizeof(ControlFileData))));
	InplaceUpgradeGetPosture(&current, ".", &posture);
	report_unreadable_posture(&posture);
	if (!crc_ok)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("incorrect checksum in control file")));
	if (posture.posture == UPGRADE_POSTURE_LIVE)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the in-place upgrade has gone live and cannot be rolled back")));
	taken_over = (posture.owner == UPGRADE_OWNER_NEW);

	if (access(INPLACE_UPGRADE_CONTROL_STASH, F_OK) == 0)
	{
		/*
		 * The old version must be able to start from the stashed control
		 * file, so the translator checks it as the takeover did.
		 */
		read_control_file(INPLACE_UPGRADE_CONTROL_STASH, &stashed);
		if (!InplaceUpgradeTranslateControlFile(&stashed))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("file \"%s\" is not a control file of an old version shut down at an upgrade boundary",
							INPLACE_UPGRADE_CONTROL_STASH)));
		if (!taken_over &&
			memcmp(&current, &stashed, sizeof(ControlFileData)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("the control file has changed since the in-place upgrade began"),
					 errdetail("The old version has run on the data directory since.")));

		nremoved = remove_created_files();

		/*
		 * With the old control file in place, the new version has not run, or
		 * an earlier rollback has restored the other files already.
		 */
		if (taken_over)
		{
			restore_relation_maps();

			/*
			 * The new version discards the old statistics file and writes its
			 * own at shutdown.
			 */
			if (access(INPLACE_UPGRADE_STATS_STASH, F_OK) == 0)
				restore_file(INPLACE_UPGRADE_STATS_STASH,
							 PGSTAT_STAT_PERMANENT_FILENAME);
			else if (unlink(PGSTAT_STAT_PERMANENT_FILENAME) == 0)
				fsync_fname(PGSTAT_STAT_PERMANENT_DIRECTORY, true);
			else if (errno != ENOENT)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m",
								PGSTAT_STAT_PERMANENT_FILENAME)));

			restore_file(INPLACE_UPGRADE_CONTROL_STASH, XLOG_CONTROL_FILE);
		}

		durable_unlink(INPLACE_UPGRADE_CONTROL_STASH, ERROR);
		ereport(LOG,
				(errmsg("rolled back the in-place upgrade"),
				 errdetail_plural("Removed %d file created by the upgrade.",
								  "Removed %d files created by the upgrade.",
								  nremoved, nremoved)));
	}
	else if (taken_over)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("could not find the stashed control file \"%s\"",
						INPLACE_UPGRADE_CONTROL_STASH)));

	if (!rmtree(INPLACE_UPGRADE_DIR, true))
		ereport(ERROR,
				(errmsg("could not remove directory \"%s\"",
						INPLACE_UPGRADE_DIR)));
	fsync_fname(".", true);

	proc_exit(0);
}

/*
 * Read the fields of a control file, without checking it.
 */
static void
read_control_file(const char *path, ControlFileData *cf)
{
	int			fd;
	int			r;

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	r = read(fd, cf, sizeof(ControlFileData));
	if (r < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", path)));
	if (r != sizeof(ControlFileData))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("could not read file \"%s\": read %d of %zu",
						path, r, sizeof(ControlFileData))));
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
}

/*
 * Remove every file on the list of created files, durably.  Returns how many
 * were there to remove.
 */
static int
remove_created_files(void)
{
	FILE	   *fp;
	char		line[MAXPGPATH + 1];
	char		dir[MAXPGPATH] = "";
	int			nremoved = 0;

	fp = AllocateFile(INPLACE_UPGRADE_CREATED_FILES, "r");
	if (fp == NULL)
	{
		/* the takeover stopped before it created anything */
		if (errno == ENOENT)
			return 0;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						INPLACE_UPGRADE_CREATED_FILES)));
	}

	while (fgets(line, sizeof(line), fp) != NULL)
	{
		char		parent[MAXPGPATH];

		line[strcspn(line, "\n")] = '\0';
		if (line[0] == '\0')
			continue;

		if (unlink(line) == 0)
			nremoved++;
		else if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", line)));

		/* Files of one directory are listed together; sync each once. */
		strlcpy(parent, line, sizeof(parent));
		get_parent_directory(parent);
		if (strcmp(parent, dir) != 0)
		{
			if (dir[0] != '\0')
				fsync_fname(dir, true);
			strlcpy(dir, parent, sizeof(dir));
		}
	}
	if (ferror(fp))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m",
						INPLACE_UPGRADE_CREATED_FILES)));
	FreeFile(fp);
	if (dir[0] != '\0')
		fsync_fname(dir, true);

	return nremoved;
}

/*
 * Put back the relation map files, if the migration saved them.  It saves
 * them before it changes any, and writes the list of databases last.
 */
static void
restore_relation_maps(void)
{
	int			ndbs;
	Oid		   *dboids;
	char	  **dbpaths;
	char		name[MAXPGPATH];
	char		path[MAXPGPATH];

	if (!InplaceUpgradeReadDatabaseList(true, &ndbs, &dboids, &dbpaths))
		return;

	snprintf(name, sizeof(name), "%s/%s.global",
			 INPLACE_UPGRADE_DIR, RELMAPPER_FILENAME);
	snprintf(path, sizeof(path), "global/%s", RELMAPPER_FILENAME);
	restore_file(name, path);

	for (int i = 0; i < ndbs; i++)
	{
		snprintf(name, sizeof(name), "%s/%s.%u",
				 INPLACE_UPGRADE_DIR, RELMAPPER_FILENAME, dboids[i]);
		snprintf(path, sizeof(path), "%s/%s", dbpaths[i], RELMAPPER_FILENAME);
		restore_file(name, path);
	}
}

/*
 * Put a copy from the bundle back in place of a file, durably and
 * atomically.
 */
static void
restore_file(const char *name, const char *path)
{
	char		tmppath[MAXPGPATH];

	snprintf(tmppath, sizeof(tmppath), "%s.tmp", path);
	if (unlink(tmppath) < 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", tmppath)));
	copy_file(name, tmppath);
	durable_rename(tmppath, path, ERROR);
}
