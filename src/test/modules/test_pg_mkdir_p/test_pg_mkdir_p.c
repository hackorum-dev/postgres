/*-------------------------------------------------------------------------
 *
 * test_pg_mkdir_p.c
 *    Helper function to test concurrent call to pg_mkdir_p()
 *
 * Copyright (c) 2007-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_pg_mkdir_p/test_pg_mkdir_p.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <pthread.h>

#include "common/file_perm.h"
#include "fmgr.h"
#include "port.h"
#include "utils/elog.h"

PG_MODULE_MAGIC;

#define TESTDIR "/tmp/testdir_pg_mkdir_p"
#define DATADIR TESTDIR "/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t/u/v/w/x/y/z"

/*
 * A struct to pass arguments to the thread and return the results.
 */
typedef struct
{
	pthread_t	tid;				/* thread id */
	char		path[MAXPGPATH];	/* the path to create */
	int			retcode;			/* return code of pg_mkdir_p() */
	int			error;				/* errno */
} Job;

PG_FUNCTION_INFO_V1(test_pg_mkdir_p);

static void *
job_thread(void *arg)
{
	Job		   *job = (Job *) arg;

	errno = 0;

	job->retcode = pg_mkdir_p(job->path, pg_dir_create_mode);
	job->error = errno;

	return NULL;
}

/*
 * This function accepts one int32 argument n, it will launch n concurrent
 * threads to call pg_mkdir_p() to create the same dir and check for errors
 * from them.
 *
 * Return true if all the calls to pg_mkdir_p() succeed, otherwise false is
 * returned.
 */
Datum
test_pg_mkdir_p(PG_FUNCTION_ARGS)
{
	int			n = PG_GETARG_INT32(0);
	int			failed = 0;
	int			i;
	Job		   *jobs;

	if (n <= 0)
		elog(ERROR, "invalid argument: %d", n);

	jobs = palloc(sizeof(Job) * n);

	rmtree(TESTDIR, true);

	/* Create concurrent threads to execute pg_mkdir_p() */
	for (i = 0; i < n; i++)
	{
		Job		   *job = &jobs[i];

		strncpy(job->path, DATADIR, sizeof(job->path));
		pthread_create(&job->tid, NULL, job_thread, job);
	}

	/* Check for the results */
	for (i = 0; i < n; i++)
	{
		Job		   *job = &jobs[i];

		pthread_join(job->tid, NULL);

		if (job->retcode < 0)
		{
			elog(NOTICE,
				 "job %d: could not create directory \"%s\": %s",
				 i, job->path, strerror(job->error));

			failed++;
		}
	}

	pfree(jobs);

	PG_RETURN_BOOL(failed == 0);
}
