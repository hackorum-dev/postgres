/*-------------------------------------------------------------------------
 *
 * golive.c
 *		Take an in-place major version upgrade live
 *
 * Take the upgrade live, once the catalogs are migrated.  This is the point
 * of no return.  Write the go-live marker first, so that any rollback is
 * refused from then on.  Then stamp the new major version into PG_VERSION,
 * in the data directory and in every database directory, and remove the
 * stale recycled WAL segments.  Last, the go-live checkpoint ends upgrade
 * mode (see CreateCheckPoint).
 *
 * PG_VERSION is stamped before the checkpoint because a backend that starts
 * after upgrade mode ends accepts only the new version there.  After a crash
 * in between, the server comes back up in upgrade mode, which accepts either
 * version, and the next call finishes the job.
 *
 * Returns the end of the go-live checkpoint record.  The checkpointer saves
 * it in the bundle before it ends upgrade mode, so a later call, even after
 * a restart, returns the same value.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/backend/upgrade/golive.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "common/inplace_upgrade_posture.h"
#include "common/relpath.h"
#include "postmaster/bgwriter.h"
#include "storage/fd.h"
#include "upgrade/bundle.h"
#include "upgrade/inplace_upgrade.h"

static XLogRecPtr read_golive_lsn(void);
static void stamp_version_files(void);
static void stamp_tablespace(const char *spcpath);
static void stamp_databases_in(const char *parent);
static bool is_oid_name(const char *name);
static void stamp_version_file(const char *dir);
static void remove_unwritten_segments(void);

/*
 * Take the upgrade live and return the end of the go-live checkpoint
 * record.  Called again, returns it again.
 */
XLogRecPtr
InplaceUpgradeGoLive(void)
{
	XLogRecPtr	lsn;
	InplaceUpgradePosture posture;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("An in-place upgrade goes live on the primary.")));

	if (!GetInplaceUpgradeMode())
	{
		lsn = GetInplaceUpgradeGoLiveLSN();
		if (!XLogRecPtrIsValid(lsn))
			lsn = read_golive_lsn();
		if (XLogRecPtrIsValid(lsn))
			return lsn;
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("no in-place upgrade is in progress")));
	}

	InplaceUpgradeServerPosture(&posture);
	if (posture.posture == UPGRADE_POSTURE_LIVE)
		ereport(LOG,
				(errmsg("resuming an interrupted go-live of the in-place upgrade")));
	else if (posture.posture == UPGRADE_POSTURE_MIGRATED)
		InplaceUpgradeWriteMarker(INPLACE_UPGRADE_MARKER_GOLIVE);
	else
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the catalogs have not been migrated")));

	stamp_version_files();
	remove_unwritten_segments();

	/*
	 * A refused request raises an error here; the checkpointer lets this one
	 * through in upgrade mode.
	 */
	RequestCheckpoint(CHECKPOINT_UPGRADE_GOLIVE | CHECKPOINT_FAST |
					  CHECKPOINT_FORCE | CHECKPOINT_WAIT);

	lsn = GetInplaceUpgradeGoLiveLSN();
	if (!XLogRecPtrIsValid(lsn))
		elog(ERROR, "the go-live checkpoint did not end the in-place upgrade");
	return lsn;
}

/*
 * Save the end of the go-live checkpoint record in the bundle, durably.
 *
 * The checkpointer calls this inside the go-live checkpoint's critical
 * section, before it writes the control file that ends upgrade mode.  So
 * this must not allocate memory, and any failure is a PANIC, after which the
 * server restarts in upgrade mode.  The write need not be atomic, because
 * the file is read only after the control file says the upgrade has gone
 * live.
 */
void
InplaceUpgradeWriteGoLiveLSN(XLogRecPtr lsn)
{
	char		line[32];
	int			len;
	int			fd;

	len = snprintf(line, sizeof(line), "%X/%08X\n", LSN_FORMAT_ARGS(lsn));
	fd = BasicOpenFile(INPLACE_UPGRADE_GOLIVE_LSN,
					   O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						INPLACE_UPGRADE_GOLIVE_LSN)));
	errno = 0;
	if (write(fd, line, len) != len)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						INPLACE_UPGRADE_GOLIVE_LSN)));
	}
	if (pg_fsync(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						INPLACE_UPGRADE_GOLIVE_LSN)));
	if (close(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						INPLACE_UPGRADE_GOLIVE_LSN)));

	/* fsync_fname() could allocate a file descriptor slot */
	fd = BasicOpenFile(INPLACE_UPGRADE_DIR, O_RDONLY | PG_BINARY);
	if (fd < 0 || pg_fsync(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync directory \"%s\": %m",
						INPLACE_UPGRADE_DIR)));
	close(fd);
}

/*
 * Where the upgrade went live, as the go-live checkpoint left it in the
 * bundle; invalid if it is not there.
 */
static XLogRecPtr
read_golive_lsn(void)
{
	FILE	   *fp;
	uint32		hi;
	uint32		lo;

	fp = AllocateFile(INPLACE_UPGRADE_GOLIVE_LSN, "r");
	if (fp == NULL)
	{
		if (errno == ENOENT)
			return InvalidXLogRecPtr;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						INPLACE_UPGRADE_GOLIVE_LSN)));
	}
	if (fscanf(fp, "%X/%X", &hi, &lo) != 2)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid contents in file \"%s\"",
						INPLACE_UPGRADE_GOLIVE_LSN)));
	FreeFile(fp);
	return ((uint64) hi << 32) | lo;
}

/*
 * Give PG_VERSION the new major version in every database directory, and in
 * the data directory last.  In a tablespace, they are under every version
 * directory there is: the old version's is not the one this server would
 * name.
 */
static void
stamp_version_files(void)
{
	DIR		   *dir;
	struct dirent *de;

	stamp_databases_in("base");

	dir = AllocateDir(PG_TBLSPC_DIR);
	while ((de = ReadDir(dir, PG_TBLSPC_DIR)) != NULL)
	{
		char		path[MAXPGPATH];

		if (!is_oid_name(de->d_name))
			continue;
		snprintf(path, sizeof(path), PG_TBLSPC_DIR "/%s", de->d_name);
		stamp_tablespace(path);
	}
	FreeDir(dir);

	stamp_version_file(".");
}

/*
 * Stamp PG_VERSION in the database directories under every version
 * directory of a tablespace.
 */
static void
stamp_tablespace(const char *spcpath)
{
	DIR		   *dir;
	struct dirent *de;

	dir = AllocateDir(spcpath);
	while ((de = ReadDir(dir, spcpath)) != NULL)
	{
		char		path[MAXPGPATH];

		if (strncmp(de->d_name, "PG_", 3) != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", spcpath, de->d_name);
		stamp_databases_in(path);
	}
	FreeDir(dir);
}

/*
 * Stamp PG_VERSION in every database directory directly below parent.
 */
static void
stamp_databases_in(const char *parent)
{
	DIR		   *dir;
	struct dirent *de;

	dir = AllocateDir(parent);
	while ((de = ReadDir(dir, parent)) != NULL)
	{
		char		path[MAXPGPATH];

		if (!is_oid_name(de->d_name))
			continue;
		snprintf(path, sizeof(path), "%s/%s", parent, de->d_name);
		stamp_version_file(path);
	}
	FreeDir(dir);
}

static bool
is_oid_name(const char *name)
{
	return strspn(name, "0123456789") == strlen(name);
}

/*
 * Replace a PG_VERSION file with one naming this server's major version,
 * durably and atomically: every start reads the file, so it must never be
 * seen empty or half-written.
 */
static void
stamp_version_file(const char *dir)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];
	FILE	   *fp;

	snprintf(path, sizeof(path), "%s/PG_VERSION", dir);
	snprintf(tmppath, sizeof(tmppath), "%s.tmp", path);
	fp = AllocateFile(tmppath, PG_BINARY_W);
	if (fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmppath)));
	if (fprintf(fp, "%s\n", PG_MAJORVERSION) < 0 || fflush(fp) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", tmppath)));
	if (pg_fsync(fileno(fp)) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmppath)));
	if (FreeFile(fp) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmppath)));
	durable_rename(tmppath, path, ERROR);
}

/*
 * Remove the segments below the one being written that hold no WAL of their
 * own: the old version recycled them ahead of its boundary segment, and the
 * takeover wrote the start segment after them, so they hold stale WAL under
 * the names of segments that were never written.  They had to stay for a
 * rollback; past the go-live marker there is none, and the archiver, which
 * resumes at go-live, must not get them.
 */
static void
remove_unwritten_segments(void)
{
	XLogSegNo	endsegno;
	DIR		   *xldir;
	struct dirent *xlde;
	int			nremoved = 0;

	XLByteToSeg(GetXLogWriteRecPtr(), endsegno, wal_segment_size);

	xldir = AllocateDir(XLOGDIR);
	while ((xlde = ReadDir(xldir, XLOGDIR)) != NULL)
	{
		TimeLineID	tli;
		XLogSegNo	segno;
		XLogRecPtr	segstart;
		char		path[MAXPGPATH];
		XLogPageHeaderData hdr;
		int			fd;
		int			r;

		if (!IsXLogFileName(xlde->d_name))
			continue;
		XLogFromFileName(xlde->d_name, &tli, &segno, wal_segment_size);
		if (segno >= endsegno)
			continue;
		XLogSegNoOffsetToRecPtr(segno, 0, wal_segment_size, segstart);

		snprintf(path, sizeof(path), XLOGDIR "/%s", xlde->d_name);
		fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
		if (fd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", path)));
		r = read(fd, &hdr, sizeof(hdr));
		if (r != sizeof(hdr))
		{
			if (r < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read file \"%s\": %m", path)));
			else
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("could not read file \"%s\": read %d of %zu",
								path, r, sizeof(hdr))));
		}
		if (CloseTransientFile(fd) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m", path)));

		/*
		 * Only the page address: the boundary segment, written by the old
		 * version, can carry another page magic.
		 */
		if (hdr.xlp_pageaddr == segstart)
			continue;
		if (unlink(path) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", path)));
		nremoved++;
	}
	FreeDir(xldir);

	if (nremoved > 0)
	{
		fsync_fname(XLOGDIR, true);
		ereport(LOG,
				(errmsg_plural("removed %d write-ahead log segment that was never written",
							   "removed %d write-ahead log segments that were never written",
							   nremoved, nremoved)));
	}
}
