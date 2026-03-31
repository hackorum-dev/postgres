/*-------------------------------------------------------------------------
 *
 * version.c
 *	 Returns the PostgreSQL version string
 *
 * Copyright (c) 1998-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *
 * src/backend/utils/adt/version.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc_tables.h"


/*
 * The version that this cluster exposes under `SELECT version()`.
 */
typedef struct PgVersionControl
{
	LWLock		lock;
	int			size;
	int			version_num;
	char		version_short[PG_CACHE_LINE_SIZE];
	char		version[FLEXIBLE_ARRAY_MEMBER];
} PgVersionControl;

static PgVersionControl *versionCtl;

static struct config_generic *VERSION_GUC;
static struct config_generic *VERSION_NUM_GUC;

#define GENERATION_IS_LOCKED(generation) ((generation & 1) == 1)

Datum
pgsql_version(PG_FUNCTION_ARGS)
{
	char	   *version;

	version = GetCurrentVersionStr();

	PG_RETURN_TEXT_P(cstring_to_text(version));
}

Datum
pgsql_update_version(PG_FUNCTION_ARGS)
{
	int			version_int;
	text	   *version_short;
	text	   *version_str;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
		ereport(ERROR, errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED));

	version_int = PG_GETARG_INT32(0);
	version_short = PG_GETARG_TEXT_P(1);
	version_str = PG_GETARG_TEXT_P(2);

	SetCurrentVersion(version_int, version_short, version_str);

	PG_RETURN_TEXT_P(version_short);
}

void
SetCurrentVersion(int version_num, text *version_short, text *version_str)
{
	Size		newlen = VARSIZE_ANY_EXHDR(version_str);
	Size		shortlen = VARSIZE_ANY_EXHDR(version_short);

	if (!superuser())
		ereport(ERROR, errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("Forbidden operation"));

	if (newlen >= versionCtl->size)
		ereport(ERROR, errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				errmsg("Version string is too long"),
				errdetail("Versions up to %d are supported", versionCtl->size));
	if (shortlen >= PG_CACHE_LINE_SIZE)
		ereport(ERROR, errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				errmsg("Version string is too long"),
				errdetail("Versions up to %d are supported", versionCtl->size));

	LWLockAcquire(&versionCtl->lock, LW_EXCLUSIVE);
	versionCtl->version_num = version_num;
	memcpy(versionCtl->version_short, VARDATA_ANY(version_short), shortlen);
	versionCtl->version_short[shortlen] = 0;
	memcpy(versionCtl->version, VARDATA_ANY(version_str), newlen);
	versionCtl->version[newlen] = 0;
	LWLockRelease(&versionCtl->lock);
}

char *
GetCurrentVersionStr(void)
{
	char	   *version = palloc0(versionCtl->size + 1);

	LWLockAcquire(&versionCtl->lock, LW_SHARED);
	strncpy(version, versionCtl->version, versionCtl->size);
	LWLockRelease(&versionCtl->lock);

	return version;
}

Size
VersionCtlShmemSize(void)
{
	Size	size = MAXALIGN(offsetof(PgVersionControl, version));

	/* keep some margin*/
	size += mul_size(MAXALIGN(strlen(PG_VERSION_STR)), 4);

	return Max(size, PG_CACHE_LINE_SIZE * 2);
}

void
VersionCtlShmemInit(void)
{
	bool		found;
	Size		size = VersionCtlShmemSize();

	versionCtl = ShmemInitStruct("VersionCtl", size, &found);

	if (!found)
	{
		LWLockInitialize(&versionCtl->lock, LWTRANCHE_VERSION_CTL);
		versionCtl->size = size - offsetof(PgVersionControl, version);


		memset(versionCtl->version, 0, versionCtl->size);
		versionCtl->size -= 1; /* guarantee 0 byte at end of version */
		memset(versionCtl->version_short, 0, PG_CACHE_LINE_SIZE);

		versionCtl->version_num = PG_VERSION_NUM;
		strncpy(versionCtl->version_short, PG_VERSION, PG_CACHE_LINE_SIZE);
		strncpy(versionCtl->version, PG_VERSION_STR, versionCtl->size);

		VERSION_GUC = find_option("server_version", false, false, LOG);
		VERSION_NUM_GUC = find_option("server_version_num", false, false,
									  LOG);

		*(VERSION_GUC->_string.variable) = versionCtl->version_short;
		VERSION_NUM_GUC->_int.variable = &versionCtl->version_num;
		VERSION_NUM_GUC->_int.min = 0;
		VERSION_NUM_GUC->_int.max = INT_MAX;
	}
}
