/*-------------------------------------------------------------------------
 *
 * controldata_utils.c
 *		Common code for control data file output.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/controldata_utils.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "catalog/pg_control.h"
#include "common/controldata_utils.h"
#include "port/pg_crc32c.h"

static ControlFileData* __get_controlfile(const char* progname);


/*
 * get_controlfile(char *DataDir, const char *progname, bool *crc_ok_p)
 *
 * Get controlfile values.  The result is returned as a palloc'd copy of the
 * control file data.
 *
 * crc_ok_p can be used by the caller to see whether the CRC of the control
 * file data is correct.
 */
ControlFileData *
get_controlfile(const char *DataDir, const char *progname, bool *crc_ok_p)
{
	ControlFileData *ControlFile;
	int			fd;
	char		ControlFilePath[MAXPGPATH];
	pg_crc32c	crc;

	AssertArg(crc_ok_p);

	ControlFile = palloc(sizeof(ControlFileData));
	snprintf(ControlFilePath, MAXPGPATH, "%s/global/pg_control", DataDir);

	if ((fd = open(ControlFilePath, O_RDONLY | PG_BINARY, 0)) == -1)
#ifndef FRONTEND
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for reading: %m",
						ControlFilePath)));
#else
	{
		fprintf(stderr, _("%s: could not open file \"%s\" for reading: %s\n"),
				progname, ControlFilePath, strerror(errno));
		exit(EXIT_FAILURE);
	}
#endif

	if (read(fd, ControlFile, sizeof(ControlFileData)) != sizeof(ControlFileData))
#ifndef FRONTEND
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", ControlFilePath)));
#else
	{
		fprintf(stderr, _("%s: could not read file \"%s\": %s\n"),
				progname, ControlFilePath, strerror(errno));
		exit(EXIT_FAILURE);
	}
#endif

	close(fd);

	/* Check the CRC. */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc,
				(char *) ControlFile,
				offsetof(ControlFileData, crc));
	FIN_CRC32C(crc);

	*crc_ok_p = EQ_CRC32C(crc, ControlFile->crc);

	/* Make sure the control file is valid byte order. */
	if (ControlFile->pg_control_version % 65536 == 0 &&
		ControlFile->pg_control_version / 65536 != 0)
#ifndef FRONTEND
		elog(ERROR, _("byte ordering mismatch"));
#else
		printf(_("WARNING: possible byte ordering mismatch\n"
				 "The byte ordering used to store the pg_control file might not match the one\n"
				 "used by this program.  In that case the results below would be incorrect, and\n"
				 "the PostgreSQL installation would be incompatible with this data directory.\n"));
#endif

	return ControlFile;
}

ControlFileData*
__get_controlfile(const char* progname)
{
	char* pg_data;
	bool crc_ok_p;

	crc_ok_p = false;

	pg_data = getenv("PGDATA");
	if (pg_data) {
		canonicalize_path(pg_data);
		return get_controlfile(pg_data, progname, &crc_ok_p);
	} else {
		printf(_("No PGDATA defined.\n"));
		return NULL;
	}
}


/*
 * Relation block size in bytes
 */
unsigned int get_rel_blck_size(const char* progname)
{
	int blcksz;
	ControlFileData* control_data;

	blcksz = 0;

        control_data =  __get_controlfile(progname);
	if (control_data != NULL) {
		blcksz = control_data->blcksz;
		pfree(control_data);
	}

	return blcksz;
}

/*
 * Relaation file size in blocks
 */
unsigned int get_rel_file_blck(const char* progname)
{
	int relseg_size;
	ControlFileData* control_data;

	relseg_size = 0;

        control_data =  __get_controlfile(progname);
	if (control_data != NULL) {
		relseg_size = control_data->relseg_size;
		pfree(control_data);
	}

        return relseg_size;
}

/*
 * Wal file block size in bytes
 */
unsigned int get_wal_blck_size(const char* progname)
{
	int xlog_blcksz;
	ControlFileData* control_data;

	xlog_blcksz = 0;

        control_data =  __get_controlfile(progname);
	if (control_data != NULL) {
		xlog_blcksz = control_data->xlog_blcksz;
		pfree(control_data);
	}

        return xlog_blcksz;
}

/*
 * Wal file size in blocks
 */
unsigned int get_wal_file_blck(const char* progname)
{
	int wal_file_blck;
	ControlFileData* control_data;

	wal_file_blck = 0;

        control_data =  __get_controlfile(progname);
	if (control_data != NULL) {
		wal_file_blck = control_data->xlog_seg_size / control_data->xlog_blcksz;
		pfree(control_data);
	}

        return wal_file_blck;
}
