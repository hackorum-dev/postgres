/*-------------------------------------------------------------------------
 *
 * shell_archive.c
 *
 * This archiving function uses a user-specified shell command (the
 * archive_command GUC) to copy write-ahead log files.  It is used as the
 * default, but other modules may define their own custom archiving logic.
 *
 * Copyright (c) 2022-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/archive/shell_archive.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/wait.h>

#include "access/xlog.h"
#include "archive/archive_module.h"
#include "archive/shell_archive.h"
#include "common/percentrepl.h"
#include "pgstat.h"

static bool shell_archive_configured(ArchiveModuleState *state);
static bool shell_archive_file(ArchiveModuleState *state,
							   const char *file,
							   const char *path);
static bool shell_archive_files(ArchiveModuleState *state,
							   char **files,
							   char **paths,
							   int nfiles);
static void shell_archive_shutdown(ArchiveModuleState *state);

static const ArchiveModuleCallbacks shell_archive_callbacks = {
	.startup_cb = NULL,
	.check_configured_cb = shell_archive_configured,
	.archive_file_cb = shell_archive_file,
	.archive_files_cb = shell_archive_files,
	.shutdown_cb = shell_archive_shutdown
};

const ArchiveModuleCallbacks *
shell_archive_init(void)
{
	return &shell_archive_callbacks;
}

static bool
shell_archive_configured(ArchiveModuleState *state)
{
	if (XLogArchiveCommand[0] != '\0')
		return true;

	arch_module_check_errdetail("\"%s\" is not set.",
								"archive_command");
	return false;
}

static bool
run_archive_command(const char *xlogarchcmd, const char *context_info, int nfiles)
{
	int rc;
	TimestampTz start_time = GetCurrentTimestamp();

	fflush(NULL);
	pgstat_report_wait_start(WAIT_EVENT_ARCHIVE_COMMAND);
	rc = system(xlogarchcmd);
	pgstat_report_wait_end();

	if (rc != 0)
	{
		int lev = wait_result_is_any_signal(rc, true) ? FATAL : LOG;

		if (WIFEXITED(rc))
		{
			ereport(lev,
				(errmsg("archive command failed with exit code %d", WEXITSTATUS(rc)),
				 errdetail("The failed archive command was: %s", xlogarchcmd),
				 context_info ? errhint("%s", context_info) : 0));
		}
		else if (WIFSIGNALED(rc))
		{
#if defined(WIN32)
			ereport(lev,
				(errmsg("archive command was terminated by exception 0x%X", WTERMSIG(rc)),
				 errhint("See C include file \"ntstatus.h\" for a description of the hexadecimal value."),
				 errdetail("The failed archive command was: %s", xlogarchcmd),
				 context_info ? errhint("%s", context_info) : 0));
#else
			ereport(lev,
				(errmsg("archive command was terminated by signal %d: %s",
						WTERMSIG(rc), pg_strsignal(WTERMSIG(rc))),
				 errdetail("The failed archive command was: %s", xlogarchcmd),
				 context_info ? errhint("%s", context_info) : 0));
#endif
		}
		else
		{
			ereport(lev,
				(errmsg("archive command exited with unrecognized status %d", rc),
				 errdetail("The failed archive command was: %s", xlogarchcmd),
				 context_info ? errhint("%s", context_info) : 0));
		}
		return false;
	}

	return true;
}

static bool
shell_archive_file(ArchiveModuleState *state, const char *file, const char *path)
{
	char	   *xlogarchcmd;
	char	   *nativePath = NULL;
	bool	    success;

	if (path)
	{
		nativePath = pstrdup(path);
		make_native_path(nativePath);
	}

	xlogarchcmd = replace_percent_placeholders(XLogArchiveCommand,
												"archive_command", "fp",
												file, nativePath);

	ereport(DEBUG3,
			(errmsg_internal("executing archive command \"%s\"", xlogarchcmd)));

	success = run_archive_command(xlogarchcmd, file, 1);

	if (success)
		elog(DEBUG1, "archived write-ahead log file \"%s\"", file);

	pfree(xlogarchcmd);
	if (nativePath)
		pfree(nativePath);

	return success;
}


static bool
shell_archive_files(ArchiveModuleState *state, char **files, char **paths, int nfiles)
{
	StringInfoData filebuf;
	StringInfoData pathbuf;
	char nfiles_str[16];
	char *xlogarchcmd;
	bool  success;

	initStringInfo(&filebuf);
	for (int i = 0; i < nfiles; i++)
	{
		if (i > 0)
			appendStringInfoChar(&filebuf, ' ');
		appendStringInfoString(&filebuf, files[i]);
	}

	initStringInfo(&pathbuf);
	for (int i = 0; i < nfiles; i++)
	{
		char *nativePath = pstrdup(paths[i]);
		make_native_path(nativePath);

		if (i > 0)
			appendStringInfoChar(&pathbuf, ' ');
		appendStringInfoString(&pathbuf, nativePath);

		pfree(nativePath);
	}

	snprintf(nfiles_str, sizeof(nfiles_str), "%d", nfiles);

	xlogarchcmd = replace_percent_placeholders(XLogArchiveCommand,
											   "archive_command", "FPN",
											   filebuf.data, pathbuf.data, nfiles_str);

	ereport(DEBUG3,
			(errmsg_internal("executing multi-file archive command: %s", xlogarchcmd)));

	success = run_archive_command(xlogarchcmd, filebuf.data, nfiles);

	if (success)
	{
		for (int i = 0; i < nfiles; i++)
			elog(DEBUG1, "archived write-ahead log file \"%s\"", files[i]);
	}

	pfree(xlogarchcmd);
	pfree(filebuf.data);
	pfree(pathbuf.data);

	return success;
}

static void
shell_archive_shutdown(ArchiveModuleState *state)
{
	elog(DEBUG1, "archiver process shutting down");
}
