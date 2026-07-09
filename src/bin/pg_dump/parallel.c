/*-------------------------------------------------------------------------
 *
 * parallel.c
 *
 *	Parallel support for pg_dump and pg_restore
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/parallel.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * Parallel operation works like this:
 *
 * The original, leader thread calls ParallelBackupStart(), which starts
 * the desired number of worker threads, which each enter WaitForCommands().
 *
 * The leader thread dispatches an individual work item to one of the worker
 * threads in DispatchJobForTocEntry().  We send a command string such as
 * "DUMP 1234" or "RESTORE 1234", where 1234 is the TocEntry ID.
 * The worker receives and decodes the command and passes it to the
 * routine pointed to by AH->WorkerJobDumpPtr or AH->WorkerJobRestorePtr,
 * which are routines of the current archive format.  That routine performs
 * the required action (dump or restore) and returns an integer status code.
 * This is passed back to the leader where we pass it to the
 * ParallelCompletionPtr callback function that was passed to
 * DispatchJobForTocEntry().  The callback function does state updating
 * for the leader control logic in pg_backup_archiver.c.
 *
 * Commands and responses are exchanged through a small in-process channel in
 * each worker's ParallelSlot (see below), protected by msg_lock.  The leader
 * and workers are all threads in the same process.
 *
 * In principle additional archive-format-specific information might be needed
 * in commands or worker status responses, but so far that hasn't proved
 * necessary, since workers have full copies of the ArchiveHandle/TocEntry
 * data structures.  Remember that we have started the workers only after we
 * have read in the catalog.  That's why our worker threads can also access
 * the catalog information.  To avoid problems, workers operate on cloned
 * copies of the Archive data structure; see RunWorker().
 *
 * The workerStatus field for each worker is only accessed by the leader, and
 * has one of the following values:
 *		WRKR_NOT_STARTED: we've not yet started this worker
 *		WRKR_IDLE: it's waiting for a command
 *		WRKR_WORKING: it's working on a command
 *		WRKR_TERMINATED: worker thread ended
 * The pstate->te[] entry for each worker is valid when it's in WRKR_WORKING
 * state, and must be NULL in other states.
 */

#include "postgres_fe.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "fe_utils/string_utils.h"
#include "parallel.h"
#include "pg_backup_utils.h"
#include "port/pg_threads.h"

#define NO_SLOT (-1)			/* Failure result for GetIdleWorker() */

/* Worker thread statuses */
typedef enum
{
	WRKR_NOT_STARTED = 0,
	WRKR_IDLE,
	WRKR_WORKING,
	WRKR_TERMINATED,
} T_WorkerStatus;

#define WORKER_IS_RUNNING(workerStatus) \
	((workerStatus) == WRKR_IDLE || (workerStatus) == WRKR_WORKING)

/*
 * Private per-parallel-worker state (typedef for this is in parallel.h).
 *
 * Much of this is valid only in the leader thread.  But the AH field should
 * be touched only by the owning worker thread.
 */
struct ParallelSlot
{
	T_WorkerStatus workerStatus;	/* see enum above */

	/* These fields are valid if workerStatus == WRKR_WORKING: */
	ParallelCompletionPtr callback; /* function to call on completion */
	void	   *callback_data;	/* passthrough data for it */

	ArchiveHandle *AH;			/* Archive data worker is using */

	/*
	 * In-process channel used to exchange messages between the leader and
	 * this worker.  A message is a malloc'd string; ownership passes to the
	 * receiver.  All fields are protected by msg_lock.
	 */
	char	   *cmdMsg;			/* command pending for the worker, or NULL */
	char	   *respMsg;		/* response pending for the leader, or NULL */
	bool		chanClosed;		/* leader closed the command channel (EOF) */
	bool		workerDied;		/* worker exited without sending a response */

	pg_thrd_t	thread;			/* worker thread identity */
};

/*
 * Structure to hold info passed to a newly-started worker thread via its
 * single allowed argument.
 */
typedef struct
{
	ArchiveHandle *AH;			/* leader database connection */
	ParallelSlot *slot;			/* this worker's parallel slot */
} WorkerInfo;

/*
 * State info for archive_close_connection() shutdown callback.
 */
typedef struct ShutdownInformation
{
	ParallelState *pstate;
	Archive    *AHX;
} ShutdownInformation;

static ShutdownInformation shutdown_info;

/*
 * State info for signal handling.
 * We assume signal_info initializes to zeroes.
 *
 * Since the workers are threads in the leader process, there's only one
 * instance of signal_info: myAH is the leader connection, and the worker
 * connections must be dug out of pstate->parallelSlot[].
 */
typedef struct DumpSignalInformation
{
	ArchiveHandle *myAH;		/* database connection to issue cancel for */
	ParallelState *pstate;		/* parallel state, if any */
	bool		handler_set;	/* signal handler set up in this process? */
} DumpSignalInformation;

static volatile DumpSignalInformation signal_info;

static pg_mtx_t signal_info_lock = PG_MTX_INIT;

/*
 * Synchronization for the in-process channels (see struct ParallelSlot).
 * msg_lock protects the per-slot cmdMsg/respMsg/chanClosed/workerDied fields;
 * worker_cv wakes a worker, leader_cv wakes the leader.
 */
static pg_mtx_t msg_lock = PG_MTX_INIT;
static pg_cnd_t worker_cv;
static pg_cnd_t leader_cv;

/*
 * Write a simple string to stderr --- must be safe in a signal handler.
 * We ignore the write() result since there's not much we could do about it.
 * Certain compilers make that harder than it ought to be.
 */
#define write_stderr(str) \
	do { \
		const char *str_ = (str); \
		int		rc_; \
		rc_ = write(fileno(stderr), str_, strlen(str_)); \
		(void) rc_; \
	} while (0)


/* Pointer to each worker thread's ParallelSlot.  NULL in the leader thread. */
static thread_local ParallelSlot *parallel_slot_thread_local;

/* Set once init_parallel_dump_utils() has run (needed by exit_nicely). */
static bool parallel_init_done = false;

/* Local function prototypes */
static void archive_close_connection(int code, void *arg);
static void ShutdownWorkersHard(ParallelState *pstate);
static void WaitForTerminatingWorkers(ParallelState *pstate);
static void handle_async_cancellation(void);
static void set_cancel_handler(void);
static void set_cancel_pstate(ParallelState *pstate);
static void set_cancel_slot_archive(ParallelSlot *slot, ArchiveHandle *AH);
static void RunWorker(ArchiveHandle *AH, ParallelSlot *slot);
static int	GetIdleWorker(ParallelState *pstate);
static void lockTableForWorker(ArchiveHandle *AH, TocEntry *te);
static void WaitForCommands(ArchiveHandle *AH, ParallelSlot *slot);
static bool ListenToWorkers(ArchiveHandle *AH, ParallelState *pstate,
							bool do_wait);
static char *getMessageFromLeader(ParallelSlot *slot);
static void sendMessageToLeader(ParallelSlot *slot, const char *str);
static char *getMessageFromWorker(ParallelState *pstate,
								  bool do_wait, int *worker);
static void sendMessageToWorker(ParallelState *pstate,
								int worker, const char *str);

#define messageStartsWith(msg, prefix) \
	(strncmp(msg, prefix, strlen(prefix)) == 0)


/*
 * Initialize parallel dump support --- should be called early in process
 * startup.  (Currently, this is called whether or not we intend parallel
 * activity.)
 */
void
init_parallel_dump_utils(void)
{
	if (!parallel_init_done)
	{
#ifdef WIN32
		WSADATA		wsaData;
		int			err;

		/* Initialize socket access */
		err = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (err != 0)
			pg_fatal("%s() failed: error code %d", "WSAStartup", err);
#endif

		/* Initialize the in-process message-channel condition variables */
		pg_cnd_init(&worker_cv);
		pg_cnd_init(&leader_cv);

		parallel_init_done = true;
	}
}

/*
 * Returns true in a parallel worker thread, false in the leader thread.
 *
 * Exported for use by exit_nicely().
 */
bool
am_parallel_worker_thread(void)
{
	return parallel_init_done && parallel_slot_thread_local != NULL;
}

/*
 * pg_dump and pg_restore call this to register the cleanup handler
 * as soon as they've created the ArchiveHandle.
 */
void
on_exit_close_archive(Archive *AHX)
{
	shutdown_info.AHX = AHX;
	on_exit_nicely(archive_close_connection, &shutdown_info);
}

/*
 * on_exit_nicely handler for shutting down database connections and
 * worker processes cleanly.
 */
static void
archive_close_connection(int code, void *arg)
{
	ShutdownInformation *si = (ShutdownInformation *) arg;

	if (si->pstate)
	{
		/* In parallel mode, must figure out who we are */
		ParallelSlot *slot = parallel_slot_thread_local;

		if (!slot)
		{
			/*
			 * We're the leader.  Forcibly shut down workers, then close our
			 * own database connection, if any.
			 */
			ShutdownWorkersHard(si->pstate);

			if (si->AHX)
				DisconnectDatabase(si->AHX);
		}
		else
		{
			/*
			 * We're a worker.  Shut down our own DB connection if any.
			 */
			if (slot->AH)
				DisconnectDatabase(&(slot->AH->public));

			/*
			 * Tell the leader we're gone so it stops waiting for our reply.
			 * The in-process channel has no EOF condition, and exit_nicely()
			 * ends only this thread, so we must signal the leader explicitly.
			 */
			pg_mtx_lock(&msg_lock);
			slot->workerDied = true;
			pg_cnd_broadcast(&leader_cv);
			pg_mtx_unlock(&msg_lock);
		}
	}
	else
	{
		/* Non-parallel operation: just kill the leader DB connection */
		if (si->AHX)
			DisconnectDatabase(si->AHX);
	}
}

/*
 * Forcibly shut down any remaining workers, waiting for them to finish.
 *
 * Note that we don't expect to come here during normal exit (the workers
 * should be long gone, and the ParallelState too).  We're only here in a
 * pg_fatal() situation, so intervening to cancel active commands is
 * appropriate.
 */
static void
ShutdownWorkersHard(ParallelState *pstate)
{
	int			i;

	/*
	 * Tell any workers that are waiting for commands that they can exit by
	 * closing their command channels.
	 */
	pg_mtx_lock(&msg_lock);
	for (i = 0; i < pstate->numWorkers; i++)
		pstate->parallelSlot[i].chanClosed = true;
	pg_cnd_broadcast(&worker_cv);
	pg_mtx_unlock(&msg_lock);

	/*
	 * Force early termination of any commands currently in progress by
	 * sending query cancels directly to the workers' backends.  Use
	 * signal_info_lock to ensure worker threads don't change the AH pointers
	 * concurrently.
	 */
	pg_mtx_lock(&signal_info_lock);
	for (i = 0; i < pstate->numWorkers; i++)
	{
		ArchiveHandle *AH = pstate->parallelSlot[i].AH;
		char		errbuf[1];

		if (AH != NULL && AH->connCancel != NULL)
			(void) PQcancel(AH->connCancel, errbuf, sizeof(errbuf));
	}
	pg_mtx_unlock(&signal_info_lock);

	/* Now wait for them to terminate. */
	WaitForTerminatingWorkers(pstate);
}

/*
 * Wait for all workers to terminate.
 */
static void
WaitForTerminatingWorkers(ParallelState *pstate)
{
	for (int i = 0; i < pstate->numWorkers; i++)
	{
		ParallelSlot *slot = &pstate->parallelSlot[i];
		int			status;

		/*
		 * Join every worker that was actually started (i.e. is IDLE or
		 * WORKING).  Skip slots that never started or were already reaped;
		 * their thread handle is not valid to join.
		 */
		if (WORKER_IS_RUNNING(slot->workerStatus))
		{
			if (pg_thrd_join(slot->thread, &status) != pg_thrd_success)
				pg_fatal("could not join worker thread %d", i);
			slot->workerStatus = WRKR_TERMINATED;
			pstate->te[i] = NULL;
		}
	}
}


/*
 * Code for responding to cancel interrupts (SIGINT, control-C, etc)
 *
 * This doesn't quite belong in this module, but it needs access to the
 * ParallelState data, so there's not really a better place either.
 *
 * When we get a cancel interrupt, we could just die, but in pg_restore that
 * could leave a SQL command (e.g., CREATE INDEX on a large table) running
 * for a long time.  Instead, we try to send a cancel request and then die.
 * pg_dump probably doesn't really need this, but we might as well use it
 * there too.  Note that sending the cancel directly from the signal handler
 * is safe because PQcancel() is written to make it so.
 *
 * The workers are threads in the leader process on all platforms, so the
 * cancel is handled the same way everywhere, in handle_async_cancellation().
 */

/*
 * Common cancellation logic for the Unix signal handler and the Windows
 * console handler.
 *
 * Unix: runs in a signal handler (async-signal-safe operations only).
 *
 * Windows: runs in a system-provided thread with signal_info_lock held by
 * the caller.
 */
static void
handle_async_cancellation(void)
{
	char		errbuf[1];

	/*
	 * Tell worker threads to stay quiet about the query cancellations we're
	 * about to send them; otherwise they'd report them as errors and clutter
	 * the user's screen.  This must be set before we send any cancel, so that
	 * a worker is guaranteed to see it by the time its query fails as a
	 * result.
	 */
	set_cancel_in_progress();

	/*
	 * If in parallel mode, send QueryCancel to each worker's connected
	 * backend.  Do this before canceling the main transaction, else we might
	 * get invalid-snapshot errors reported before we can stop the workers.
	 * Ignore errors, there's not much we can do about them anyway.
	 */
	if (signal_info.pstate != NULL)
	{
		for (int i = 0; i < signal_info.pstate->numWorkers; i++)
		{
			ArchiveHandle *AH = signal_info.pstate->parallelSlot[i].AH;

			if (AH != NULL && AH->connCancel != NULL)
				(void) PQcancel(AH->connCancel, errbuf, sizeof(errbuf));
		}
	}

	/*
	 * Send QueryCancel to leader connection, if enabled.  Ignore errors,
	 * there's not much we can do about them anyway.
	 */
	if (signal_info.myAH != NULL && signal_info.myAH->connCancel != NULL)
		(void) PQcancel(signal_info.myAH->connCancel, errbuf, sizeof(errbuf));

	/*
	 * Report we're quitting, using nothing more complicated than write(2).
	 */
	if (progname)
	{
		write_stderr(progname);
		write_stderr(": ");
	}
	write_stderr("terminated by user\n");
}

#ifndef WIN32

/*
 * Signal handler (Unix only)
 */
static void
sigTermHandler(SIGNAL_ARGS)
{
	/*
	 * Some platforms allow delivery of new signals to interrupt an active
	 * signal handler.  That could muck up our attempt to send PQcancel, so
	 * disable the signals that set_cancel_handler enabled.
	 */
	pqsignal(SIGINT, PG_SIG_IGN);
	pqsignal(SIGTERM, PG_SIG_IGN);
	pqsignal(SIGQUIT, PG_SIG_IGN);

	handle_async_cancellation();

	/*
	 * And die, using _exit() not exit() because the latter will invoke atexit
	 * handlers that can fail if we interrupted related code.
	 */
	_exit(1);
}

/*
 * Enable cancel interrupt handler, if not already done.
 */
static void
set_cancel_handler(void)
{
	if (!signal_info.handler_set)
	{
		signal_info.handler_set = true;

		pqsignal(SIGINT, sigTermHandler);
		pqsignal(SIGTERM, sigTermHandler);
		pqsignal(SIGQUIT, sigTermHandler);
	}
}

#else							/* WIN32 */

/*
 * Console interrupt handler --- runs in a newly-started thread.
 *
 * After sending cancel requests on all open connections, we return FALSE
 * which will allow the default ExitProcess() action to be taken.
 */
static BOOL WINAPI
consoleHandler(DWORD dwCtrlType)
{
	if (dwCtrlType == CTRL_C_EVENT ||
		dwCtrlType == CTRL_BREAK_EVENT)
	{
		/* Critical section prevents changing data we look at here */
		pg_mtx_lock(&signal_info_lock);
		handle_async_cancellation();
		pg_mtx_unlock(&signal_info_lock);
	}

	/* Always return FALSE to allow signal handling to continue */
	return FALSE;
}

/*
 * Enable cancel interrupt handler, if not already done.
 */
static void
set_cancel_handler(void)
{
	if (!signal_info.handler_set)
	{
		signal_info.handler_set = true;

		SetConsoleCtrlHandler(consoleHandler, TRUE);
	}
}

#endif							/* WIN32 */


/*
 * set_archive_cancel_info
 *
 * Fill AH->connCancel with cancellation info for the specified database
 * connection; or clear it if conn is NULL.
 */
void
set_archive_cancel_info(ArchiveHandle *AH, PGconn *conn)
{
	PGcancel   *oldConnCancel;

	/*
	 * Activate the interrupt handler if we didn't yet in this process.
	 */
	set_cancel_handler();

	/*
	 * Serialize updates to the cancel pointers under signal_info_lock; on
	 * Windows this also interlocks against the console-handler thread.
	 */
	pg_mtx_lock(&signal_info_lock);

	/* Free the old one if we have one */
	oldConnCancel = AH->connCancel;
	/* be sure interrupt handler doesn't use pointer while freeing */
	AH->connCancel = NULL;

	if (oldConnCancel != NULL)
		PQfreeCancel(oldConnCancel);

	/* Set the new one if specified */
	if (conn)
		AH->connCancel = PQgetCancel(conn);

	/*
	 * Set the leader's myAH, unless we're in a worker thread.  Workers make
	 * sure their ArchiveHandle appears in the pstate data, which is dealt
	 * with in RunWorker().
	 */
	if (parallel_slot_thread_local == NULL)
		signal_info.myAH = AH;

	pg_mtx_unlock(&signal_info_lock);
}

/*
 * set_cancel_pstate
 *
 * Set signal_info.pstate to point to the specified ParallelState, if any.
 * We need this mainly to have an interlock against Windows signal thread.
 */
static void
set_cancel_pstate(ParallelState *pstate)
{
	pg_mtx_lock(&signal_info_lock);
	signal_info.pstate = pstate;
	pg_mtx_unlock(&signal_info_lock);
}

/*
 * set_cancel_slot_archive
 *
 * Set ParallelSlot's AH field to point to the specified archive, if any.
 * We need this mainly to have an interlock against Windows signal thread.
 */
static void
set_cancel_slot_archive(ParallelSlot *slot, ArchiveHandle *AH)
{
	pg_mtx_lock(&signal_info_lock);
	slot->AH = AH;
	pg_mtx_unlock(&signal_info_lock);
}


/*
 * This function is called to set up and run a worker thread.  Caller should
 * exit the thread upon return.
 */
static void
RunWorker(ArchiveHandle *AH, ParallelSlot *slot)
{
	/*
	 * Clone the archive so that we have our own state to work with, and in
	 * particular our own database connection.  CloneArchive resets the state
	 * information and also clones the database connection, both of which are
	 * essential since worker threads share the leader's address space.
	 */
	AH = CloneArchive(AH);

	/* Remember cloned archive where signal handler can find it */
	set_cancel_slot_archive(slot, AH);

	/*
	 * Call the setup worker function that's defined in the ArchiveHandle.
	 */
	(AH->SetupWorkerPtr) ((Archive *) AH);

	/*
	 * Execute commands until done.
	 */
	WaitForCommands(AH, slot);

	/*
	 * Disconnect from database and clean up.
	 */
	set_cancel_slot_archive(slot, NULL);
	DisconnectDatabase(&(AH->public));
	DeCloneArchive(AH);
}

/*
 * Thread start function for all platforms.  Matches pg_thrd_start_t.
 */
static int
worker_thread_main(void *argument)
{
	WorkerInfo *wi = argument;
	ArchiveHandle *AH = wi->AH;
	ParallelSlot *slot = wi->slot;

	/* Don't need WorkerInfo anymore */
	free(wi);

	/*
	 * Record our slot so that archive_close_connection() and
	 * am_parallel_worker_thread() can tell us from the leader.  Do this
	 * before anything that might call exit_nicely(): the cleanup handler uses
	 * this pointer, and mistaking a failing worker for the leader deadlocks
	 * shutdown.
	 */
	parallel_slot_thread_local = slot;

	/* Run the worker ... */
	RunWorker(AH, slot);

	/* Exit the thread */
	return 0;
}

/*
 * This function starts a parallel dump or restore by spawning off the worker
 * threads.
 */
ParallelState *
ParallelBackupStart(ArchiveHandle *AH)
{
	ParallelState *pstate;
	int			i;

	Assert(AH->public.numWorkers > 0);

	pstate = pg_malloc_object(ParallelState);

	pstate->numWorkers = AH->public.numWorkers;
	pstate->te = NULL;
	pstate->parallelSlot = NULL;

	if (AH->public.numWorkers == 1)
		return pstate;

	/* Create status arrays, being sure to initialize all fields to 0 */
	pstate->te =
		pg_malloc0_array(TocEntry *, pstate->numWorkers);
	pstate->parallelSlot =
		pg_malloc0_array(ParallelSlot, pstate->numWorkers);

	/*
	 * Set the pstate in shutdown_info, to tell the exit handler that it must
	 * clean up workers as well as the main database connection.  But we don't
	 * set this in signal_info yet, because until the workers have something
	 * to do we want a cancel to just kill the leader connection.
	 */
	shutdown_info.pstate = pstate;

	/*
	 * Temporarily disable query cancellation on the leader connection.  No
	 * harm is done if we fail while it's disabled, because the leader
	 * connection is idle at this point anyway.
	 */
	set_archive_cancel_info(AH, NULL);

	/* Ensure stdio state is quiesced before starting workers */
	fflush(NULL);

	/* Create desired number of workers */
	for (i = 0; i < pstate->numWorkers; i++)
	{
		WorkerInfo *wi;
		ParallelSlot *slot = &(pstate->parallelSlot[i]);

		/* Create transient structure to pass args to worker function */
		wi = pg_malloc_object(WorkerInfo);
		wi->AH = AH;
		wi->slot = slot;

		if (pg_thrd_create(&slot->thread, worker_thread_main, wi) !=
			pg_thrd_success)
			pg_fatal("could not create worker thread");

		slot->workerStatus = WRKR_IDLE;
	}

	/*
	 * Having started the workers, disable SIGPIPE so that the process isn't
	 * killed if the leader tries to write to a backend over a broken
	 * connection.  (libpq still uses sockets even though our own worker
	 * communication no longer does.)
	 */
#ifndef WIN32
	pqsignal(SIGPIPE, PG_SIG_IGN);
#endif

	/*
	 * Re-establish query cancellation on the leader connection.
	 */
	set_archive_cancel_info(AH, AH->connection);

	/*
	 * Tell the cancel signal handler about the workers so it can cancel their
	 * backends too.  (As with query cancel, we did not need this earlier
	 * because the workers have not yet been given anything to do; if we die
	 * before this point, any already-started workers will see their command
	 * channels close and quit promptly.)
	 */
	set_cancel_pstate(pstate);

	return pstate;
}

/*
 * Close down a parallel dump or restore.
 */
void
ParallelBackupEnd(ArchiveHandle *AH, ParallelState *pstate)
{
	int			i;

	/* No work if non-parallel */
	if (pstate->numWorkers == 1)
		return;

	/* There should not be any unfinished jobs */
	Assert(IsEveryWorkerIdle(pstate));

	/* Tell the workers they can exit by closing their command channels */
	pg_mtx_lock(&msg_lock);
	for (i = 0; i < pstate->numWorkers; i++)
		pstate->parallelSlot[i].chanClosed = true;
	pg_cnd_broadcast(&worker_cv);
	pg_mtx_unlock(&msg_lock);

	/* Wait for them to exit */
	WaitForTerminatingWorkers(pstate);

	/*
	 * Unlink pstate from shutdown_info, so the exit handler will not try to
	 * use it; and likewise unlink from signal_info.
	 */
	shutdown_info.pstate = NULL;
	set_cancel_pstate(NULL);

	/* Release state (mere neatnik-ism, since we're about to terminate) */
	free(pstate->te);
	free(pstate->parallelSlot);
	free(pstate);
}

/*
 * These next four functions handle construction and parsing of the command
 * strings and response strings for parallel workers.
 *
 * Currently, these can be the same regardless of which archive format we are
 * processing.  In future, we might want to let format modules override these
 * functions to add format-specific data to a command or response.
 */

/*
 * buildWorkerCommand: format a command string to send to a worker.
 *
 * The string is built in the caller-supplied buffer of size buflen.
 */
static void
buildWorkerCommand(ArchiveHandle *AH, TocEntry *te, T_Action act,
				   char *buf, int buflen)
{
	if (act == ACT_DUMP)
		snprintf(buf, buflen, "DUMP %d", te->dumpId);
	else if (act == ACT_RESTORE)
		snprintf(buf, buflen, "RESTORE %d", te->dumpId);
	else
		Assert(false);
}

/*
 * parseWorkerCommand: interpret a command string in a worker.
 */
static void
parseWorkerCommand(ArchiveHandle *AH, TocEntry **te, T_Action *act,
				   const char *msg)
{
	DumpId		dumpId;
	int			nBytes;

	if (messageStartsWith(msg, "DUMP "))
	{
		*act = ACT_DUMP;
		sscanf(msg, "DUMP %d%n", &dumpId, &nBytes);
		Assert(nBytes == strlen(msg));
		*te = getTocEntryByDumpId(AH, dumpId);
		Assert(*te != NULL);
	}
	else if (messageStartsWith(msg, "RESTORE "))
	{
		*act = ACT_RESTORE;
		sscanf(msg, "RESTORE %d%n", &dumpId, &nBytes);
		Assert(nBytes == strlen(msg));
		*te = getTocEntryByDumpId(AH, dumpId);
		Assert(*te != NULL);
	}
	else
		pg_fatal("unrecognized command received from leader: \"%s\"",
				 msg);
}

/*
 * buildWorkerResponse: format a response string to send to the leader.
 *
 * The string is built in the caller-supplied buffer of size buflen.
 */
static void
buildWorkerResponse(ArchiveHandle *AH, TocEntry *te, T_Action act, int status,
					char *buf, int buflen)
{
	snprintf(buf, buflen, "OK %d %d %d",
			 te->dumpId,
			 status,
			 status == WORKER_IGNORED_ERRORS ? AH->public.n_errors : 0);
}

/*
 * parseWorkerResponse: parse the status message returned by a worker.
 *
 * Returns the integer status code, and may update fields of AH and/or te.
 */
static int
parseWorkerResponse(ArchiveHandle *AH, TocEntry *te,
					const char *msg)
{
	DumpId		dumpId;
	int			nBytes,
				n_errors;
	int			status = 0;

	if (messageStartsWith(msg, "OK "))
	{
		sscanf(msg, "OK %d %d %d%n", &dumpId, &status, &n_errors, &nBytes);

		Assert(dumpId == te->dumpId);
		Assert(nBytes == strlen(msg));

		AH->public.n_errors += n_errors;
	}
	else
		pg_fatal("invalid message received from worker: \"%s\"",
				 msg);

	return status;
}

/*
 * Dispatch a job to some free worker.
 *
 * te is the TocEntry to be processed, act is the action to be taken on it.
 * callback is the function to call on completion of the job.
 *
 * If no worker is currently available, this will block, and previously
 * registered callback functions may be called.
 */
void
DispatchJobForTocEntry(ArchiveHandle *AH,
					   ParallelState *pstate,
					   TocEntry *te,
					   T_Action act,
					   ParallelCompletionPtr callback,
					   void *callback_data)
{
	int			worker;
	char		buf[256];

	/* Get a worker, waiting if none are idle */
	while ((worker = GetIdleWorker(pstate)) == NO_SLOT)
		WaitForWorkers(AH, pstate, WFW_ONE_IDLE);

	/* Construct and send command string */
	buildWorkerCommand(AH, te, act, buf, sizeof(buf));

	sendMessageToWorker(pstate, worker, buf);

	/* Remember worker is busy, and which TocEntry it's working on */
	pstate->parallelSlot[worker].workerStatus = WRKR_WORKING;
	pstate->parallelSlot[worker].callback = callback;
	pstate->parallelSlot[worker].callback_data = callback_data;
	pstate->te[worker] = te;
}

/*
 * Find an idle worker and return its slot number.
 * Return NO_SLOT if none are idle.
 */
static int
GetIdleWorker(ParallelState *pstate)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
	{
		if (pstate->parallelSlot[i].workerStatus == WRKR_IDLE)
			return i;
	}
	return NO_SLOT;
}

/*
 * Return true iff every worker is in the WRKR_IDLE state.
 */
bool
IsEveryWorkerIdle(ParallelState *pstate)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
	{
		if (pstate->parallelSlot[i].workerStatus != WRKR_IDLE)
			return false;
	}
	return true;
}

/*
 * Acquire lock on a table to be dumped by a worker process.
 *
 * The leader process is already holding an ACCESS SHARE lock.  Ordinarily
 * it's no problem for a worker to get one too, but if anything else besides
 * pg_dump is running, there's a possible deadlock:
 *
 * 1) Leader dumps the schema and locks all tables in ACCESS SHARE mode.
 * 2) Another process requests an ACCESS EXCLUSIVE lock (which is not granted
 *	  because the leader holds a conflicting ACCESS SHARE lock).
 * 3) A worker process also requests an ACCESS SHARE lock to read the table.
 *	  The worker is enqueued behind the ACCESS EXCLUSIVE lock request.
 * 4) Now we have a deadlock, since the leader is effectively waiting for
 *	  the worker.  The server cannot detect that, however.
 *
 * To prevent an infinite wait, prior to touching a table in a worker, request
 * a lock in ACCESS SHARE mode but with NOWAIT.  If we don't get the lock,
 * then we know that somebody else has requested an ACCESS EXCLUSIVE lock and
 * so we have a deadlock.  We must fail the backup in that case.
 */
static void
lockTableForWorker(ArchiveHandle *AH, TocEntry *te)
{
	const char *qualId;
	PQExpBuffer query;
	PGresult   *res;

	/* Nothing to do for BLOBS */
	if (strcmp(te->desc, "BLOBS") == 0)
		return;

	query = createPQExpBuffer();

	qualId = fmtQualifiedId(te->namespace, te->tag);

	appendPQExpBuffer(query, "LOCK TABLE %s IN ACCESS SHARE MODE NOWAIT",
					  qualId);

	res = PQexec(AH->connection, query->data);

	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("could not obtain lock on relation \"%s\"\n"
				 "This usually means that someone requested an ACCESS EXCLUSIVE lock "
				 "on the table after the pg_dump parent process had gotten the "
				 "initial ACCESS SHARE lock on the table.", qualId);

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * WaitForCommands: main routine for a worker thread.
 *
 * Read and execute commands from the leader until the command channel is
 * closed.
 */
static void
WaitForCommands(ArchiveHandle *AH, ParallelSlot *slot)
{
	char	   *command;
	TocEntry   *te;
	T_Action	act;
	int			status = 0;
	char		buf[256];

	for (;;)
	{
		if (!(command = getMessageFromLeader(slot)))
		{
			/* EOF, so done */
			return;
		}

		/* Decode the command */
		parseWorkerCommand(AH, &te, &act, command);

		if (act == ACT_DUMP)
		{
			/* Acquire lock on this table within the worker's session */
			lockTableForWorker(AH, te);

			/* Perform the dump command */
			status = (AH->WorkerJobDumpPtr) (AH, te);
		}
		else if (act == ACT_RESTORE)
		{
			/* Perform the restore command */
			status = (AH->WorkerJobRestorePtr) (AH, te);
		}
		else
			Assert(false);

		/* Return status to leader */
		buildWorkerResponse(AH, te, act, status, buf, sizeof(buf));

		sendMessageToLeader(slot, buf);

		/* command was pg_malloc'd and we are responsible for free()ing it. */
		free(command);
	}
}

/*
 * Check for status messages from workers.
 *
 * If do_wait is true, wait to get a status message; otherwise, just return
 * immediately if there is none available.
 *
 * When we get a status message, we pass the status code to the callback
 * function that was specified to DispatchJobForTocEntry, then reset the
 * worker status to IDLE.
 *
 * Returns true if we collected a status message, else false.
 *
 * XXX is it worth checking for more than one status message per call?
 * It seems somewhat unlikely that multiple workers would finish at exactly
 * the same time.
 */
static bool
ListenToWorkers(ArchiveHandle *AH, ParallelState *pstate, bool do_wait)
{
	int			worker;
	char	   *msg;

	/* Try to collect a status message */
	msg = getMessageFromWorker(pstate, do_wait, &worker);

	if (!msg)
	{
		/* If do_wait is true, a worker must have died without responding */
		if (do_wait)
			pg_fatal("a worker thread died unexpectedly");
		return false;
	}

	/* Process it and update our idea of the worker's status */
	if (messageStartsWith(msg, "OK "))
	{
		ParallelSlot *slot = &pstate->parallelSlot[worker];
		TocEntry   *te = pstate->te[worker];
		int			status;

		status = parseWorkerResponse(AH, te, msg);
		slot->callback(AH, te, status, slot->callback_data);
		slot->workerStatus = WRKR_IDLE;
		pstate->te[worker] = NULL;
	}
	else
		pg_fatal("invalid message received from worker: \"%s\"",
				 msg);

	/* Free the string returned from getMessageFromWorker */
	free(msg);

	return true;
}

/*
 * Check for status results from workers, waiting if necessary.
 *
 * Available wait modes are:
 * WFW_NO_WAIT: reap any available status, but don't block
 * WFW_GOT_STATUS: wait for at least one more worker to finish
 * WFW_ONE_IDLE: wait for at least one worker to be idle
 * WFW_ALL_IDLE: wait for all workers to be idle
 *
 * Any received results are passed to the callback specified to
 * DispatchJobForTocEntry.
 *
 * This function is executed in the leader process.
 */
void
WaitForWorkers(ArchiveHandle *AH, ParallelState *pstate, WFW_WaitOption mode)
{
	bool		do_wait = false;

	/*
	 * In GOT_STATUS mode, always block waiting for a message, since we can't
	 * return till we get something.  In other modes, we don't block the first
	 * time through the loop.
	 */
	if (mode == WFW_GOT_STATUS)
	{
		/* Assert that caller knows what it's doing */
		Assert(!IsEveryWorkerIdle(pstate));
		do_wait = true;
	}

	for (;;)
	{
		/*
		 * Check for status messages, even if we don't need to block.  We do
		 * not try very hard to reap all available messages, though, since
		 * there's unlikely to be more than one.
		 */
		if (ListenToWorkers(AH, pstate, do_wait))
		{
			/*
			 * If we got a message, we are done by definition for GOT_STATUS
			 * mode, and we can also be certain that there's at least one idle
			 * worker.  So we're done in all but ALL_IDLE mode.
			 */
			if (mode != WFW_ALL_IDLE)
				return;
		}

		/* Check whether we must wait for new status messages */
		switch (mode)
		{
			case WFW_NO_WAIT:
				return;			/* never wait */
			case WFW_GOT_STATUS:
				Assert(false);	/* can't get here, because we waited */
				break;
			case WFW_ONE_IDLE:
				if (GetIdleWorker(pstate) != NO_SLOT)
					return;
				break;
			case WFW_ALL_IDLE:
				if (IsEveryWorkerIdle(pstate))
					return;
				break;
		}

		/* Loop back, and this time wait for something to happen */
		do_wait = true;
	}
}

/*
 * Read one command message from the leader, blocking if necessary
 * until one is available, and return it as a malloc'd string.
 * On channel close, return NULL.
 *
 * This function is executed in worker threads.
 */
static char *
getMessageFromLeader(ParallelSlot *slot)
{
	char	   *msg;

	pg_mtx_lock(&msg_lock);
	while (slot->cmdMsg == NULL && !slot->chanClosed)
		pg_cnd_wait(&worker_cv, &msg_lock);
	msg = slot->cmdMsg;			/* NULL here means the channel was closed */
	slot->cmdMsg = NULL;
	pg_mtx_unlock(&msg_lock);
	return msg;
}

/*
 * Send a status message to the leader.
 *
 * This function is executed in worker threads.
 */
static void
sendMessageToLeader(ParallelSlot *slot, const char *str)
{
	pg_mtx_lock(&msg_lock);
	Assert(slot->respMsg == NULL);
	slot->respMsg = pg_strdup(str);
	pg_cnd_broadcast(&leader_cv);
	pg_mtx_unlock(&msg_lock);
}

/*
 * Check for messages from worker threads.
 *
 * If a message is available, return it as a malloc'd string, and put the
 * index of the sending worker in *worker.
 *
 * If nothing is available, wait if "do_wait" is true, else return NULL.
 *
 * If a worker has died without responding, we'll return NULL.  It's not great
 * that that's hard to distinguish from the no-data-available case, but for now
 * our one caller is okay with that.
 *
 * This function is executed in the leader thread.
 */
static char *
getMessageFromWorker(ParallelState *pstate, bool do_wait, int *worker)
{
	int			i;

	/*
	 * Return the first pending response; if none and do_wait, sleep on
	 * leader_cv until a worker posts one.
	 */
	pg_mtx_lock(&msg_lock);
	for (;;)
	{
		bool		anyDied = false;

		for (i = 0; i < pstate->numWorkers; i++)
		{
			char	   *msg;

			if (!WORKER_IS_RUNNING(pstate->parallelSlot[i].workerStatus))
				continue;
			msg = pstate->parallelSlot[i].respMsg;
			if (msg != NULL)
			{
				pstate->parallelSlot[i].respMsg = NULL;
				pg_mtx_unlock(&msg_lock);
				*worker = i;
				return msg;
			}
			if (pstate->parallelSlot[i].workerDied)
				anyDied = true;
		}

		/*
		 * A worker died without responding: return NULL so the caller reports
		 * the failure instead of hanging.
		 */
		if (anyDied)
		{
			pg_mtx_unlock(&msg_lock);
			return NULL;
		}
		if (!do_wait)
		{
			pg_mtx_unlock(&msg_lock);
			return NULL;
		}
		pg_cnd_wait(&leader_cv, &msg_lock);
	}
}

/*
 * Send a command message to the specified worker thread.
 *
 * This function is executed in the leader thread.
 */
static void
sendMessageToWorker(ParallelState *pstate, int worker, const char *str)
{
	ParallelSlot *slot = &pstate->parallelSlot[worker];

	pg_mtx_lock(&msg_lock);
	Assert(slot->cmdMsg == NULL);
	slot->cmdMsg = pg_strdup(str);
	pg_cnd_broadcast(&worker_cv);
	pg_mtx_unlock(&msg_lock);
}
