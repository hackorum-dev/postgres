/*-------------------------------------------------------------------------
 *
 * inplace_upgrade_posture.c
 *		Where a data directory stands in an in-place major version upgrade
 *
 * The posture of a cluster is derived from durable facts in its data
 * directory alone: the control file, the markers of the rollback bundle
 * (see upgrade/bundle.h), the error the in-place upgrade migrator left, and
 * PG_VERSION.  The server, the postmaster, and pg_inplace_upgrade all ask
 * here, so that each of them reaches the same answer the same way.
 *
 * Nothing here reports an error: input that cannot be
 * read gives the posture UPGRADE_POSTURE_UNREADABLE, with the reason in its
 * detail, and the caller reports that the way it reports anything.
 *
 * The control file of the old major version can have a layout of its own.
 * The translators below read each layout this version can upgrade from, so
 * that frontend programs read the file the way the server does.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/inplace_upgrade_posture.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/inplace_upgrade_posture.h"
#include "port/pg_crc32c.h"
#include "upgrade/bundle.h"

#ifndef FRONTEND
#include "storage/fd.h"
#endif

/*
 * The control file layouts this version can upgrade from, keyed by the old
 * version's PG_CONTROL_VERSION.  Each translator verifies the checksum in the
 * old layout and copies the contents into the current one.
 */
typedef struct OldControlFormat
{
	uint32		pg_control_version;
	bool		(*translate) (const ControlFileData *raw, ControlFileData *out);
} OldControlFormat;

static bool translate_control_2001(const ControlFileData *raw,
								   ControlFileData *out);

static const OldControlFormat old_control_formats[] = {
	{2001, translate_control_2001}, /* version 20 */
	{0, NULL}
};

static const OldControlFormat *old_control_format(uint32 pg_control_version);
static int	read_file_start(const char *path, void *buf, int len);
static bool file_exists(const char *datadir, const char *name,
						InplaceUpgradePosture *result);
static int	data_major_version(const char *datadir,
							   InplaceUpgradePosture *result);


/*
 * Translate a control file of PG_CONTROL_VERSION 2001, the current layout.
 * Returns whether its checksum is correct.
 */
static bool
translate_control_2001(const ControlFileData *raw, ControlFileData *out)
{
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, raw, offsetof(ControlFileData, crc));
	FIN_CRC32C(crc);

	/* WIP: stub identity translation -- a real one would have to do more work */
	memcpy(out, raw, sizeof(ControlFileData));

	return EQ_CRC32C(crc, raw->crc);
}

static const OldControlFormat *
old_control_format(uint32 pg_control_version)
{
	const OldControlFormat *format;

	for (format = old_control_formats; format->translate != NULL; format++)
	{
		if (format->pg_control_version == pg_control_version)
			return format;
	}
	return NULL;
}

/*
 * Translate a control file written by an old major version, in place.
 *
 * On entry, *cf holds the raw bytes of the file.  pg_control_version is at
 * the same offset in every layout, so it picks the translator.  On success,
 * *cf holds the contents in the current layout, and *crc_ok says whether
 * the checksum was correct in the old layout.  The old version numbers stay
 * in place, so pg_control_version still differs from PG_CONTROL_VERSION,
 * which marks the cluster as not yet taken over.
 *
 * Returns false, leaving *cf unchanged, if we can't upgrade from this
 * version.
 */
bool
InplaceUpgradeTranslateControl(ControlFileData *cf, bool *crc_ok)
{
	const OldControlFormat *format;
	ControlFileData translated;

	format = old_control_format(cf->pg_control_version);
	if (format == NULL)
		return false;

	*crc_ok = format->translate(cf, &translated);
	memcpy(cf, &translated, sizeof(ControlFileData));
	return true;
}

/*
 * Read a control file in the current layout, or in an old layout this
 * version can upgrade from, translating the latter (see
 * InplaceUpgradeTranslateControl).
 *
 * Returns the number of bytes read, or -1 with errno set, as read() does.
 * Only if that is sizeof(ControlFileData) are *cf and *crc_ok set.  For a
 * version whose layout we don't know, *crc_ok is false, and the posture
 * derived from *cf is UPGRADE_POSTURE_UNREADABLE.
 */
int
InplaceUpgradeReadControlFile(const char *path, ControlFileData *cf,
							  bool *crc_ok)
{
	int			r;

	r = read_file_start(path, cf, sizeof(ControlFileData));
	if (r != sizeof(ControlFileData))
		return r;

	if (cf->pg_control_version == PG_CONTROL_VERSION)
	{
		pg_crc32c	crc;

		INIT_CRC32C(crc);
		COMP_CRC32C(crc, cf, offsetof(ControlFileData, crc));
		FIN_CRC32C(crc);
		*crc_ok = EQ_CRC32C(crc, cf->crc);
	}
	else if (!InplaceUpgradeTranslateControl(cf, crc_ok))
		*crc_ok = false;

	return r;
}

/*
 * Derive the posture of the cluster in datadir from its control file, either
 * the server's copy or one read with InplaceUpgradeReadControlFile().
 *
 * The old installation owns the cluster while the control file is in the old
 * layout.  Two major versions can share a layout and its version number, so
 * the old installation also owns a cluster with a control file in the
 * current layout if it is not in upgrade mode and PG_VERSION names an
 * earlier major version.  PG_VERSION keeps naming the old version until the
 * upgrade goes live.
 *
 * A cluster owned by the old version is at a boundary if its latest
 * checkpoint is a boundary checkpoint and it shut down there.  A cluster
 * owned by the new version is live unless it is in upgrade mode, in which
 * case its markers show how far the upgrade has come.  It is live once the
 * go-live marker is written, even before upgrade mode ends.
 */
void
InplaceUpgradeGetPosture(const ControlFileData *cf, const char *datadir,
						 InplaceUpgradePosture *result)
{
	memset(result, 0, sizeof(InplaceUpgradePosture));
	result->posture = UPGRADE_POSTURE_UNREADABLE;

	if (cf->pg_control_version != PG_CONTROL_VERSION &&
		old_control_format(cf->pg_control_version) == NULL)
	{
		snprintf(result->detail, sizeof(result->detail),
				 "control file version %u is not one this version can upgrade from",
				 cf->pg_control_version);
		return;
	}

	result->owner = UPGRADE_OWNER_NEW;
	if (cf->pg_control_version != PG_CONTROL_VERSION)
		result->owner = UPGRADE_OWNER_OLD;
	else if (!cf->inplaceUpgradeMode)
	{
		int			major = data_major_version(datadir, result);

		if (major < 0)
			return;
		if (major < PG_MAJORVERSION_NUM)
			result->owner = UPGRADE_OWNER_OLD;
	}

	if (result->owner == UPGRADE_OWNER_OLD)
	{
		if (cf->checkPointCopy.upgradeFlag == UPGRADE_FLAG_BOUNDARY &&
			cf->state == DB_SHUTDOWNED)
			result->posture = UPGRADE_POSTURE_AT_BOUNDARY;
		else
			result->posture = UPGRADE_POSTURE_NOT_PREPARED;
		return;
	}

	if (!cf->inplaceUpgradeMode ||
		file_exists(datadir, INPLACE_UPGRADE_MARKER_DIR "/" INPLACE_UPGRADE_MARKER_GOLIVE, result))
		result->posture = UPGRADE_POSTURE_LIVE;
	else if (file_exists(datadir, INPLACE_UPGRADE_MARKER_DIR "/" INPLACE_UPGRADE_MARKER_MIGRATED, result))
		result->posture = UPGRADE_POSTURE_MIGRATED;
	else if (file_exists(datadir, INPLACE_UPGRADE_MIGRATE_ERROR, result))
		result->posture = UPGRADE_POSTURE_MIGRATION_FAILED;
	else if (file_exists(datadir, INPLACE_UPGRADE_MARKER_DIR "/" INPLACE_UPGRADE_MARKER_PLANNED, result))
		result->posture = UPGRADE_POSTURE_MIGRATING;
	else
		result->posture = UPGRADE_POSTURE_TAKEN_OVER;

	/* A file that could not be stat'ed counts as absent above. */
	if (result->detail[0] != '\0')
		result->posture = UPGRADE_POSTURE_UNREADABLE;
}

/*
 * Read the first len bytes of a file.  Returns what read() does, or -1 with
 * errno set if the file cannot be opened.
 */
static int
read_file_start(const char *path, void *buf, int len)
{
	int			fd;
	int			r;
	int			save_errno;

#ifndef FRONTEND
	fd = BasicOpenFile(path, O_RDONLY | PG_BINARY);
#else
	fd = open(path, O_RDONLY | PG_BINARY, 0);
#endif
	if (fd < 0)
		return -1;
	r = read(fd, buf, len);
	save_errno = errno;
	close(fd);
	errno = save_errno;
	return r;
}

/*
 * Does the file exist in the data directory?  A file that cannot be stat'ed
 * for any other reason than its absence leaves the reason in result->detail,
 * and once a reason is there, every file counts as absent.
 */
static bool
file_exists(const char *datadir, const char *name,
			InplaceUpgradePosture *result)
{
	char		path[MAXPGPATH];
	struct stat st;

	if (result->detail[0] != '\0')
		return false;
	snprintf(path, sizeof(path), "%s/%s", datadir, name);
	if (stat(path, &st) == 0)
		return true;
	if (errno != ENOENT)
		snprintf(result->detail, sizeof(result->detail),
				 "could not stat file \"%s\": %m", path);
	return false;
}

/*
 * The major version that PG_VERSION names, or -1 with the reason in
 * result->detail if the file cannot be read.
 */
static int
data_major_version(const char *datadir, InplaceUpgradePosture *result)
{
	char		path[MAXPGPATH];
	char		buf[64];
	char	   *end;
	long		major;
	int			r;

	snprintf(path, sizeof(path), "%s/PG_VERSION", datadir);
	r = read_file_start(path, buf, sizeof(buf) - 1);
	if (r < 0)
	{
		snprintf(result->detail, sizeof(result->detail),
				 "could not read file \"%s\": %m", path);
		return -1;
	}
	buf[r] = '\0';

	errno = 0;
	major = strtol(buf, &end, 10);
	if (errno != 0 || end == buf || major <= 0 || major > INT_MAX ||
		(*end != '\n' && *end != '\0'))
	{
		snprintf(result->detail, sizeof(result->detail),
				 "file \"%s\" does not contain a valid major version", path);
		return -1;
	}
	return (int) major;
}
