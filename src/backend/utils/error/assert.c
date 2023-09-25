/*-------------------------------------------------------------------------
 *
 * assert.c
 *	  Assert support code.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/error/assert.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#include <storage/dsm.h>
#include <storage/pg_shmem.h>

int core_dump_no_shared_buffers = COREDUMP_INCLUDE_ALL;

/*
 * Remember, at the same time someone can work with shared memory, write them to
 * disk and so on.
 */
void
pg_abort(void)
{
	if (core_dump_no_shared_buffers != COREDUMP_INCLUDE_ALL)
	{
		if (core_dump_no_shared_buffers == COREDUMP_EXCLUDE_ALL ||
			core_dump_no_shared_buffers == COREDUMP_EXCLUDE_DSM)
			dsm_detach_all();

		if (core_dump_no_shared_buffers == COREDUMP_EXCLUDE_ALL ||
			core_dump_no_shared_buffers == COREDUMP_EXCLUDE_SHMEM)
			PGSharedMemoryDetach();
	}

	abort();
}

/*
 * ExceptionalCondition - Handles the failure of an Assert()
 *
 * We intentionally do not go through elog() here, on the grounds of
 * wanting to minimize the amount of infrastructure that has to be
 * working to report an assertion failure.
 */
void
ExceptionalCondition(const char *conditionName,
					 const char *fileName,
					 int lineNumber)
{
	/* Report the failure on stderr (or local equivalent) */
	if (!PointerIsValid(conditionName)
		|| !PointerIsValid(fileName))
		write_stderr("TRAP: ExceptionalCondition: bad arguments in PID %d\n",
					 (int) getpid());
	else
		write_stderr("TRAP: failed Assert(\"%s\"), File: \"%s\", Line: %d, PID: %d\n",
					 conditionName, fileName, lineNumber, (int) getpid());

	/* Usually this shouldn't be needed, but make sure the msg went out */
	fflush(stderr);

	/* If we have support for it, dump a simple backtrace */
#ifdef HAVE_BACKTRACE_SYMBOLS
	{
		void	   *buf[100];
		int			nframes;

		nframes = backtrace(buf, lengthof(buf));
		backtrace_symbols_fd(buf, nframes, fileno(stderr));
	}
#endif

	/*
	 * If configured to do so, sleep indefinitely to allow user to attach a
	 * debugger.  It would be nice to use pg_usleep() here, but that can sleep
	 * at most 2G usec or ~33 minutes, which seems too short.
	 */
#ifdef SLEEP_ON_ASSERT
	sleep(1000000);
#endif

	pg_abort();
}
