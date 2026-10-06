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
 * a start checkpoint.  The server then runs in in-place upgrade mode,
 * admitting only superusers, until the upgrade goes live.
 *
 * The takeover first saves everything it replaces, and records every file it
 * creates, so that a rollback can undo it.
 *
 * Rewriting the control file is the last step and the only one that changes
 * what the next startup sees.  If the server stops before that, the cluster
 * is still at the boundary in the old format, and the next startup does the
 * takeover again.
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
static bool control_file_stashed(void);
static void read_whole_file(const char *path, char *buf, int len);
static uint64 aligned_next_fxid(FullTransactionId nextXid);
static void write_start_segment(ControlFileData *cf, char *fname);
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
			ereport(FATAL,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot take over a standby server for an in-place upgrade"),
					 errdetail("The database cluster was shut down in recovery at an upgrade boundary.")));
			break;

		default:
			elog(ERROR, "unexpected database cluster state %d at an upgrade boundary",
				 (int) cf->state);
	}

	Assert(cf->pg_control_version == PG_CONTROL_VERSION);
}

/*
 * Take over the primary.  The old version's WAL ends with the boundary
 * checkpoint.  The new version's WAL starts with a shutdown checkpoint of its
 * own, flagged START, in a new segment after every segment in pg_wal.  The
 * cluster's identity and counters carry over from the old control file,
 * except that the next transaction ID moves up to a pg_xact segment boundary
 * (see aligned_next_fxid).  The server then comes up in in-place upgrade
 * mode.
 */
static void
take_over_primary(ControlFileData *cf)
{
	char		walfname[MAXFNAMELEN];
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
	 * repeats an interrupted one.  Its WAL segment can differ from the one
	 * the interrupted takeover created, so the list keeps that one, and a
	 * rollback removes both.  Otherwise the old version may have run since
	 * and reused the names on the list, so the list starts afresh.
	 */
	if (!control_file_stashed() &&
		unlink(INPLACE_UPGRADE_CREATED_FILES) < 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m",
						INPLACE_UPGRADE_CREATED_FILES)));
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
	cf->pg_control_version = PG_CONTROL_VERSION;
	cf->catalog_version_no = CATALOG_VERSION_NO;
	cf->state = DB_SHUTDOWNED;
	cf->time = (pg_time_t) time(NULL);
	cf->inplaceUpgradeMode = true;

	nextXid = FullTransactionIdFromU64(aligned_next_fxid(cf->checkPointCopy.nextXid));

	/*
	 * WIP: need to sanity-check that migration won't overwrite existing
	 * pg_xact files.
	 */

	cf->checkPointCopy.nextXid = nextXid;

	/*
	 * The start checkpoint: the boundary checkpoint's contents, at a new
	 * location.  Its LSN is fixed by the segment chosen for it.
	 */
	write_start_segment(cf, walfname);

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
 * Write the segment holding the start checkpoint, and fill in the
 * checkpoint's location in the control file.  The segment is the first one
 * past every WAL file present, so that no old file is touched.  Its name
 * is returned in fname.
 *
 * New WAL begins with a shutdown checkpoint, as WAL from initdb or
 * pg_resetwal does: it describes a cluster with nothing running and nothing
 * to replay.  The record is built by hand, since nothing is running yet to
 * insert it.
 */
static void
write_start_segment(ControlFileData *cf, char *fname)
{
	TimeLineID	tli = cf->checkPointCopy.ThisTimeLineID;
	int			walsegsz = cf->xlog_seg_size;
	XLogSegNo	newsegno;
	DIR		   *dir;
	struct dirent *de;
	PGAlignedXLogBlock buf;
	XLogPageHeader page;
	XLogLongPageHeader longpage;
	XLogRecord *record;
	char	   *recptr;
	pg_crc32c	crc;
	char		path[MAXPGPATH];

	XLByteToSeg(cf->checkPoint, newsegno, walsegsz);
	dir = AllocateDir(XLOGDIR);
	while ((de = ReadDir(dir, XLOGDIR)) != NULL)
	{
		TimeLineID	fnametli;
		XLogSegNo	segno;

		if (!IsXLogFileName(de->d_name) &&
			!IsPartialXLogFileName(de->d_name))
			continue;
		XLogFromFileName(de->d_name, &fnametli, &segno, walsegsz);
		if (segno > newsegno)
			newsegno = segno;
	}
	FreeDir(dir);
	newsegno++;

	XLogSegNoOffsetToRecPtr(newsegno, SizeOfXLogLongPHD, walsegsz,
							cf->checkPointCopy.redo);
	cf->checkPointCopy.PrevTimeLineID = tli;
	cf->checkPointCopy.time = cf->time;
	cf->checkPointCopy.oldestActiveXid = InvalidTransactionId;
	cf->checkPointCopy.upgradeFlag = UPGRADE_FLAG_START;
	cf->checkPoint = cf->checkPointCopy.redo;

	/* the segment's first page: long header, then the checkpoint record */
	memset(buf.data, 0, XLOG_BLCKSZ);
	page = (XLogPageHeader) buf.data;
	page->xlp_magic = XLOG_PAGE_MAGIC;
	page->xlp_info = XLP_LONG_HEADER;
	page->xlp_tli = tli;
	page->xlp_pageaddr = cf->checkPointCopy.redo - SizeOfXLogLongPHD;
	longpage = (XLogLongPageHeader) page;
	longpage->xlp_sysid = cf->system_identifier;
	longpage->xlp_seg_size = walsegsz;
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

	XLogFileName(fname, tli, newsegno, walsegsz);
	snprintf(path, sizeof(path), "%s/%s", XLOGDIR, fname);
	InplaceUpgradeRegisterCreatedFile(path);
	fsync_fname(INPLACE_UPGRADE_DIR, true);
	create_or_recreate_file(path, buf.data, XLOG_BLCKSZ, walsegsz);
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
