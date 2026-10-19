/*-------------------------------------------------------------------------
 *
 * xlogupgrade.c
 *		Taking over a database cluster at an in-place major version upgrade
 *
 * An in-place upgrade starts with the old major version shutting the
 * primary down at an upgrade boundary.  The new major version is then
 * started on the same data directory, and its startup process takes over the
 * cluster before reading WAL or serving a connection.  It rewrites the
 * control file in the current format and begins the new version's WAL with
 * a start checkpoint, in the segment after the one holding the boundary
 * checkpoint record.  The server then runs in in-place upgrade mode,
 * admitting only superusers, until the upgrade goes live.
 *
 * The takeover first saves everything it replaces, and records every file it
 * creates, so that a rollback can undo it.  The exception is the WAL
 * segments from the start segment on, which the takeover removes.  The old
 * version recycled or preallocated them ahead of its boundary, so none holds
 * WAL, and they are recreated as needed.
 *
 * Rewriting the control file is the last step and the only one that changes
 * what the next startup sees.  If the server stops before that, the cluster
 * is still at the boundary in the old format, and the next startup does the
 * takeover again.
 *
 * A standby paused at the boundary is taken over once the upgrade has gone
 * live on the primary.  It writes no WAL and keeps nothing for a rollback:
 * its control file is pointed at the start checkpoint, which it computes
 * from the boundary record the same way the primary did, and it replays
 * the new version's WAL from there with consistency withheld
 * (ControlFileData.inplaceUpgradeHold) until the go-live checkpoint.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/xlogupgrade.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include "access/clog.h"
#include "access/commit_ts.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xlogupgrade.h"
#include "catalog/catversion.h"
#include "common/controldata_utils.h"
#include "common/file_perm.h"
#include "common/inplace_upgrade_posture.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "upgrade/bundle.h"
#include "upgrade/inplace_upgrade.h"

/* Transaction IDs per pg_xact segment file */
#define CLOG_XACTS_PER_SEGMENT \
	((uint64) CLOG_XACTS_PER_PAGE * SLRU_PAGES_PER_SEGMENT)

static void take_over_primary(ControlFileData *cf);
static void take_over_standby(ControlFileData *cf);
static void remove_future_segments(XLogSegNo startsegno, int walsegsz);
static bool control_file_stashed(void);
static void read_whole_file(const char *path, char *buf, int len);
static XLogSegNo start_segment(const ControlFileData *cf);
static void set_start_checkpoint(ControlFileData *cf, XLogSegNo segno);
static uint64 aligned_next_fxid(FullTransactionId nextXid);
static void write_start_segment(const ControlFileData *cf, const char *path);
static void write_pg_xact_page(FullTransactionId nextXid);
static void create_or_recreate_file(const char *path, const char *buf,
									int len, int size);


/*
 * Translate a control file written by an old major version.
 *
 * On entry *cf holds the raw bytes of the file.  On success *cf holds the
 * file's contents in the current layout, with the old version numbers left
 * in place (see InplaceUpgradeTranslateControl()).  Returns false when the
 * version is not one we can upgrade from, or the cluster is not at an
 * upgrade boundary; the caller then complains about the version mismatch as
 * usual.
 */
bool
InplaceUpgradeTranslateControlFile(ControlFileData *cf)
{
	ControlFileData translated;
	bool		crc_ok;
	InplaceUpgradePosture posture;

	memcpy(&translated, cf, sizeof(ControlFileData));
	if (!InplaceUpgradeTranslateControl(&translated, &crc_ok))
		return false;
	if (!crc_ok)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("incorrect checksum in control file")));

	InplaceUpgradeGetPosture(&translated, DataDir, &posture);
	if (posture.posture != UPGRADE_POSTURE_AT_BOUNDARY)
		return false;

	memcpy(cf, &translated, sizeof(ControlFileData));
	return true;
}

/*
 * Take over the cluster from the old major version.  Called by the startup
 * process before it does anything else with the cluster, with the translated
 * control file that was written to disk.
 */
void
InplaceUpgradeTakeover(ControlFileData *cf)
{
	Assert(cf->pg_control_version != PG_CONTROL_VERSION);

	switch (cf->state)
	{
		case DB_SHUTDOWNED:
			if (cf->track_commit_timestamp)
				ereport(FATAL,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot take over a database cluster for an in-place upgrade while \"%s\" is enabled",
								"track_commit_timestamp"),
						 errdetail("The old server ran with \"%s\" enabled.",
								   "track_commit_timestamp"),
						 errhint("Disable \"%s\", restart the old server, and shut it down at an upgrade boundary again.",
								 "track_commit_timestamp")));
			else if (track_commit_timestamp)
				ereport(FATAL,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot take over a database cluster for an in-place upgrade while \"%s\" is enabled",
								"track_commit_timestamp"),
						 errdetail("\"%s\" is enabled for this server.",
								   "track_commit_timestamp"),
						 errhint("Disable \"%s\" for the in-place upgrade.",
								 "track_commit_timestamp")));
			take_over_primary(cf);
			break;

		case DB_SHUTDOWNED_IN_RECOVERY:
			take_over_standby(cf);
			break;

		default:
			elog(ERROR, "unexpected database cluster state %d at an upgrade boundary",
				 (int) cf->state);
	}

	Assert(cf->pg_control_version == PG_CONTROL_VERSION);
}

/*
 * The segment that holds the start checkpoint: the one after the segment
 * holding the last byte of the boundary checkpoint record, which cf points
 * at.  Both roles compute it from their control file alone, so a standby
 * finds the primary's start checkpoint without being told where it is.
 */
static XLogSegNo
start_segment(const ControlFileData *cf)
{
	XLogRecPtr	pos = cf->checkPoint;
	XLogSegNo	segno;
	uint32		remaining;

	/*
	 * WIP: a real translator supplies the old version's record length; the
	 * identity translation makes it this version's.
	 */
	remaining = SizeOfXLogRecord + SizeOfXLogRecordDataHeaderShort +
		sizeof(CheckPoint);

	/* The record's end, as xlogreader computes it: skip page headers. */
	for (;;)
	{
		uint32		avail = XLOG_BLCKSZ - (pos % XLOG_BLCKSZ);

		if (remaining <= avail)
		{
			pos += remaining;
			break;
		}
		remaining -= avail;
		pos += avail;
		if (XLogSegmentOffset(pos, cf->xlog_seg_size) == 0)
			pos += SizeOfXLogLongPHD;
		else
			pos += SizeOfXLogShortPHD;
	}
	pos = MAXALIGN(pos);

	XLByteToPrevSeg(pos, segno, cf->xlog_seg_size);
	return segno + 1;
}

/*
 * Take over the primary.  The old version's WAL ends with the boundary
 * checkpoint.  The new version's WAL starts with a shutdown checkpoint of its
 * own, flagged START, in the segment after the boundary record's.  The
 * cluster's identity and counters carry over from the old control file,
 * except that the next transaction ID moves up to a pg_xact segment boundary
 * (see aligned_next_fxid).  The server then comes up in in-place upgrade
 * mode.
 */
static void
take_over_primary(ControlFileData *cf)
{
	bool		repeat;
	XLogSegNo	segno;
	char		walfname[MAXFNAMELEN];
	char		walpath[MAXPGPATH];
	FullTransactionId nextXid;

	ereport(LOG,
			(errmsg("taking over the database cluster for an in-place upgrade"),
			 errdetail("The database cluster was shut down at an upgrade boundary.")));

	/* What we replace is kept, so that the upgrade can be rolled back. */
	if (MakePGDirectory(INPLACE_UPGRADE_DIR) < 0 && errno != EEXIST)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m",
						INPLACE_UPGRADE_DIR)));

	/*
	 * A takeover that finds the control file unchanged since it was stashed
	 * repeats an interrupted one: the start segment in pg_wal is then the one
	 * it created, and it removed the segments past the start segment already.
	 * Otherwise the old version may have run since and reused the names on
	 * the list of created files, so the list starts afresh and the segments
	 * go again.
	 */
	repeat = control_file_stashed();
	segno = start_segment(cf);
	XLogFileName(walfname, cf->checkPointCopy.ThisTimeLineID, segno,
				 cf->xlog_seg_size);
	snprintf(walpath, sizeof(walpath), "%s/%s", XLOGDIR, walfname);
	if (!repeat)
	{
		if (unlink(INPLACE_UPGRADE_CREATED_FILES) < 0 && errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m",
							INPLACE_UPGRADE_CREATED_FILES)));
		remove_future_segments(segno, cf->xlog_seg_size);
		InplaceUpgradeRegisterCreatedFile(walpath);
		fsync_fname(INPLACE_UPGRADE_DIR, true);
	}

	InplaceUpgradeStashFile(XLOG_CONTROL_FILE, INPLACE_UPGRADE_CONTROL_STASH);

	/*
	 * After the takeover, startup discards the statistics file because it
	 * can't read the old version's format.  A rollback needs the file back,
	 * so stash it first.
	 */
	if (access(PGSTAT_STAT_PERMANENT_FILENAME, F_OK) == 0)
		InplaceUpgradeStashFile(PGSTAT_STAT_PERMANENT_FILENAME,
								INPLACE_UPGRADE_STATS_STASH);

	/* The control file's new contents */
	set_start_checkpoint(cf, segno);
	cf->state = DB_SHUTDOWNED;
	cf->inplaceUpgradeMode = true;
	cf->inplaceUpgradeHold = false;
	nextXid = cf->checkPointCopy.nextXid;

	/*
	 * WIP: need to sanity-check that migration won't overwrite existing
	 * pg_xact files.
	 */

	/*
	 * The start checkpoint: the boundary checkpoint's contents, at a new
	 * location.
	 */
	write_start_segment(cf, walpath);

	/*
	 * If the next xid, after skipping the reserved transaction IDs, is not on
	 * a page boundary, the first startup reads its pg_xact page before
	 * anything has zeroed it.  Create the page here.
	 */
	if (U64FromFullTransactionId(nextXid) % CLOG_XACTS_PER_PAGE != 0)
		write_pg_xact_page(nextXid);

	/*
	 * Lastly, the control file.  Once this is durable, the cluster is in the
	 * new version's format.
	 */
	update_controlfile(DataDir, cf, true);

	ereport(LOG,
			(errmsg("took over the database cluster for an in-place upgrade"),
			 errdetail("The new version's WAL starts at %X/%08X in segment %s; the next transaction ID is %u:%u.",
					   LSN_FORMAT_ARGS(cf->checkPoint), walfname,
					   EpochFromFullTransactionId(nextXid),
					   XidFromFullTransactionId(nextXid))));
}

/*
 * The standby's takeover.  It happens only once the upgrade has gone live
 * on the primary, so there is no rollback and nothing is kept.  The control
 * file is pointed at the start checkpoint, which the primary wrote in the
 * segment after the boundary record's, and recovery fetches that segment
 * from the primary or the archive like any other; the walreceiver streams
 * from the segment's start, so a stale copy of it in pg_wal is overwritten.
 * The recovery fields stay as they are: minRecoveryPoint lies at or before
 * the boundary, and the consistency hold keeps the standby from opening
 * before the go-live checkpoint.  The standby never enters in-place upgrade
 * mode: nothing of the migration happens here, it is all in the WAL.
 *
 * PG_VERSION is stamped now.  The postmaster of this start accepted the old
 * version in it because the control file was in the old layout; a restart
 * mid-crossing would not, and no backend reads a database's PG_VERSION
 * before the hold is released.
 */
static void
take_over_standby(ControlFileData *cf)
{
	ereport(LOG,
			(errmsg("taking over the standby server for an in-place upgrade"),
			 errdetail("The database cluster was shut down in recovery at an upgrade boundary.")));

	set_start_checkpoint(cf, start_segment(cf));
	cf->inplaceUpgradeHold = true;

	InplaceUpgradeStampVersionFiles();

	update_controlfile(DataDir, cf, true);

	ereport(LOG,
			(errmsg("took over the standby server for an in-place upgrade"),
			 errdetail("Replay starts at the new version's start checkpoint at %X/%08X.",
					   LSN_FORMAT_ARGS(cf->checkPoint))));
}

/*
 * Remove the segments from the start segment on from pg_wal.  The old
 * version wrote nothing past its boundary checkpoint, so they are segments
 * it recycled or preallocated ahead of it, and the new version's WAL would
 * overwrite them from the start checkpoint on.  They are not kept for a
 * rollback: the old version never read them and preallocates anew when it
 * runs again, as it does after every start with wal_recycle off.  Every
 * segment the new version writes from here on is one it created, which the
 * rollback removes.
 */
static void
remove_future_segments(XLogSegNo startsegno, int walsegsz)
{
	DIR		   *dir;
	struct dirent *de;

	dir = AllocateDir(XLOGDIR);
	while ((de = ReadDir(dir, XLOGDIR)) != NULL)
	{
		TimeLineID	tli;
		XLogSegNo	segno;
		char		path[MAXPGPATH];

		if (!IsXLogFileName(de->d_name))
			continue;
		XLogFromFileName(de->d_name, &tli, &segno, walsegsz);
		if (segno < startsegno)
			continue;
		snprintf(path, sizeof(path), XLOGDIR "/%s", de->d_name);
		durable_unlink(path, ERROR);
	}
	FreeDir(dir);
}

/*
 * Is the stashed control file a copy of the current one?
 */
static bool
control_file_stashed(void)
{
	char		current[PG_CONTROL_FILE_SIZE];
	char		stashed[PG_CONTROL_FILE_SIZE];

	if (access(INPLACE_UPGRADE_CONTROL_STASH, F_OK) != 0)
		return false;
	read_whole_file(XLOG_CONTROL_FILE, current, PG_CONTROL_FILE_SIZE);
	read_whole_file(INPLACE_UPGRADE_CONTROL_STASH, stashed,
					PG_CONTROL_FILE_SIZE);
	return memcmp(current, stashed, PG_CONTROL_FILE_SIZE) == 0;
}

static void
read_whole_file(const char *path, char *buf, int len)
{
	int			fd;
	int			r;

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	r = read(fd, buf, len);
	if (r != len)
	{
		if (r < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", path)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %d",
							path, r, len)));
	}
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
}

/*
 * Turn the translated control file into the new version's: the boundary
 * checkpoint's contents become the start checkpoint, at the start of the
 * given segment, with the next transaction ID moved up (see
 * aligned_next_fxid).  Both roles arrive at the same start checkpoint from
 * the same boundary this way.
 */
static void
set_start_checkpoint(ControlFileData *cf, XLogSegNo segno)
{
	cf->pg_control_version = PG_CONTROL_VERSION;
	cf->catalog_version_no = CATALOG_VERSION_NO;
	cf->time = (pg_time_t) time(NULL);
	cf->checkPointCopy.nextXid =
		FullTransactionIdFromU64(aligned_next_fxid(cf->checkPointCopy.nextXid));
	XLogSegNoOffsetToRecPtr(segno, SizeOfXLogLongPHD, cf->xlog_seg_size,
							cf->checkPointCopy.redo);
	cf->checkPoint = cf->checkPointCopy.redo;
	cf->checkPointCopy.PrevTimeLineID = cf->checkPointCopy.ThisTimeLineID;
	cf->checkPointCopy.time = cf->time;
	cf->checkPointCopy.oldestActiveXid = InvalidTransactionId;
	cf->checkPointCopy.upgradeFlag = UPGRADE_FLAG_START;
}

/*
 * The next transaction ID for the new version: the old one rounded up to the
 * start of a pg_xact segment.
 *
 * The migration allocates transaction IDs before the upgrade goes live.
 * After a rollback, the old version allocates the same IDs again, so their
 * commit status must not survive anywhere the old version looks.  Starting
 * at a segment boundary puts every status bit written from now on in pg_xact
 * segment files that did not exist before, so a rollback only has to remove
 * files, not restore their contents.
 *
 * TODO: make sure they don't exist first.
 *
 * This skips at most one segment of IDs.  When the boundary is the first
 * page of an epoch, step over the reserved special IDs, as the allocator
 * does.
 */
static uint64
aligned_next_fxid(FullTransactionId nextXid)
{
	uint64		nextfxid = U64FromFullTransactionId(nextXid);

	nextfxid += CLOG_XACTS_PER_SEGMENT - 1;
	nextfxid -= nextfxid % CLOG_XACTS_PER_SEGMENT;
	if (XidFromFullTransactionId(FullTransactionIdFromU64(nextfxid)) <
		FirstNormalTransactionId)
		nextfxid += FirstNormalTransactionId;
	return nextfxid;
}

/*
 * Write the segment holding the start checkpoint, at the given path, from
 * the checkpoint's location and contents in the control file.
 *
 * New WAL begins with a shutdown checkpoint, as WAL from initdb or
 * pg_resetwal does: it describes a cluster with nothing running and nothing
 * to replay.  The record is built by hand, since nothing is running yet to
 * insert it.
 */
static void
write_start_segment(const ControlFileData *cf, const char *path)
{
	PGAlignedXLogBlock buf;
	XLogPageHeader page;
	XLogLongPageHeader longpage;
	XLogRecord *record;
	char	   *recptr;
	pg_crc32c	crc;

	/* the segment's first page: long header, then the checkpoint record */
	memset(buf.data, 0, XLOG_BLCKSZ);
	page = (XLogPageHeader) buf.data;
	page->xlp_magic = XLOG_PAGE_MAGIC;
	page->xlp_info = XLP_LONG_HEADER;
	page->xlp_tli = cf->checkPointCopy.ThisTimeLineID;
	page->xlp_pageaddr = cf->checkPoint - SizeOfXLogLongPHD;
	longpage = (XLogLongPageHeader) page;
	longpage->xlp_sysid = cf->system_identifier;
	longpage->xlp_seg_size = cf->xlog_seg_size;
	longpage->xlp_xlog_blcksz = XLOG_BLCKSZ;

	recptr = (char *) page + SizeOfXLogLongPHD;
	record = (XLogRecord *) recptr;
	record->xl_prev = InvalidXLogRecPtr;
	record->xl_xid = InvalidTransactionId;
	record->xl_tot_len = SizeOfXLogRecord +
		SizeOfXLogRecordDataHeaderShort + sizeof(CheckPoint);
	record->xl_info = XLOG_CHECKPOINT_SHUTDOWN;
	record->xl_rmid = RM_XLOG_ID;
	recptr += SizeOfXLogRecord;
	*(recptr++) = (char) XLR_BLOCK_ID_DATA_SHORT;
	*(recptr++) = sizeof(CheckPoint);
	memcpy(recptr, &cf->checkPointCopy, sizeof(CheckPoint));

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, ((char *) record) + SizeOfXLogRecord,
				record->xl_tot_len - SizeOfXLogRecord);
	COMP_CRC32C(crc, (char *) record, offsetof(XLogRecord, xl_crc));
	FIN_CRC32C(crc);
	record->xl_crc = crc;

	create_or_recreate_file(path, buf.data, XLOG_BLCKSZ, cf->xlog_seg_size);
	fsync_fname(XLOGDIR, true);
}

/*
 * Write the zeroed first page of the pg_xact segment the next transaction
 * ID falls in.
 */
static void
write_pg_xact_page(FullTransactionId nextXid)
{
	PGAlignedBlock zeroes;
	char		fname[16];
	char		path[MAXPGPATH];

	memset(zeroes.data, 0, BLCKSZ);
	snprintf(fname, sizeof(fname), "%04X",
			 (uint32) (XidFromFullTransactionId(nextXid) / CLOG_XACTS_PER_SEGMENT));
	snprintf(path, sizeof(path), "pg_xact/%s", fname);
	InplaceUpgradeRegisterCreatedFile(path);
	fsync_fname(INPLACE_UPGRADE_DIR, true);
	create_or_recreate_file(path, zeroes.data, BLCKSZ, BLCKSZ);
	fsync_fname("pg_xact", true);
}

/*
 * Create a file of the given size holding buf at its start and zeroes
 * after, replacing whatever a previous, interrupted takeover left there.
 * The caller fsyncs the directory.
 *
 * The zeroes go out in WAL-page-sized chunks; only the WAL segment needs
 * filling, and its writer works in those units, but nothing here depends on
 * the size.
 */
static void
create_or_recreate_file(const char *path, const char *buf, int len, int size)
{
	int			fd;
	PGAlignedXLogBlock zeroes;

	if (unlink(path) < 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", path)));

	fd = OpenTransientFile(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", path)));

	errno = 0;
	if (write(fd, buf, len) != len)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", path)));
	}

	memset(zeroes.data, 0, XLOG_BLCKSZ);
	while (len < size)
	{
		int			nbytes = Min(size - len, XLOG_BLCKSZ);

		errno = 0;
		if (write(fd, zeroes.data, nbytes) != nbytes)
		{
			if (errno == 0)
				errno = ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m", path)));
		}
		len += nbytes;
	}

	if (pg_fsync(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
}
