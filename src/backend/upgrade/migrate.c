/*-------------------------------------------------------------------------
 *
 * migrate.c
 *		Migrating the catalogs at an in-place major version upgrade
 *
 * After the takeover at startup (see xlogupgrade.c), the server runs the
 * new major version on the catalogs of the old one, in in-place upgrade
 * mode.  The in-place upgrade migrator, a child of the postmaster, migrates
 * the catalogs of every database.  The postmaster starts it whenever the
 * server enters normal operation in upgrade mode with the catalogs not yet
 * migrated, and until they are, admits no connection and starts no other
 * process that reads the catalogs (see InplaceUpgradeCatalogsUsable() in
 * postmaster.c).  The migrator itself has no database connection and reads
 * no catalog through the relation cache: it reads them page by page through
 * the buffer manager, with copies of the old version's row layouts (see
 * upgrade/catalog_20.h).
 *
 * The migrator first checks for conditions that the migration cannot
 * handle, and refuses with the data directory unchanged if it finds one.
 * Then it saves anything needed for rollback (see bundle.c).
 *
 * The migrator reports each step in the server log and in its process
 * title.  An error ends it, and the postmaster then shuts down; the error
 * is also left in INPLACE_UPGRADE_MIGRATE_ERROR, for a program driving the
 * upgrade to read without parsing the server log.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/upgrade/migrate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "access/twophase.h"
#include "access/xlog.h"
#include "catalog/pg_tablespace_d.h"
#include "common/inplace_upgrade_posture.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/postmaster.h"
#include "replication/logicalctl.h"
#include "replication/slot.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "upgrade/inplace_upgrade.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/relmapper.h"
#include "utils/resowner.h"
#include "utils/wait_event.h"

static void migrate_catalogs(void);
static void migrator_shmem_exit(int code, Datum arg);
static void write_error_file(void);
static void refuse_prepared_transactions(void);
static void refuse_replication_slots(void);
static void save_for_rollback(List *databases);


/*
 * Main entry point of the in-place upgrade migrator
 */
void
InplaceUpgradeMigratorMain(const void *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;
	InplaceUpgradePosture posture;

	Assert(startup_data_len == 0);

	/* Release postmaster's working memory context */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	init_ps_display(NULL);

	Assert(GetProcessingMode() == InitProcessing);

	/*
	 * Set up signal handlers.  A shutdown request of any kind terminates us
	 * at once; SIGQUIT was set up by InitPostmasterChild.
	 */
	pqsignal(SIGHUP, PG_SIG_IGN);
	pqsignal(SIGINT, PG_SIG_IGN);
	pqsignal(SIGTERM, die);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, PG_SIG_IGN);
	pqsignal(SIGPIPE, PG_SIG_IGN);
	pqsignal(SIGCHLD, PG_SIG_DFL);

	/*
	 * Create a per-backend PGPROC struct in shared memory.  We must do this
	 * before we access any shared memory.
	 */
	InitProcess();

	/*
	 * Early initialization.  We don't connect to a database, so we do by hand
	 * what InitPostgres would do for the shared parts.
	 */
	BaseInit();

	HOLD_INTERRUPTS();
	ProcSignalInit(NULL, 0);
	InitLocalDataChecksumState();
	RESUME_INTERRUPTS();
	InitializeProcessXLogLogicalInfo();

	/*
	 * There is no transaction, so buffer pins need a resource owner of our
	 * own, and nothing but us releases our lightweight locks at exit.
	 */
	CreateAuxProcessResourceOwner();
	before_shmem_exit(migrator_shmem_exit, 0);

	pgstat_beinit();
	pgstat_bestart_initial();
	pgstat_bestart_final();

	SetProcessingMode(NormalProcessing);

	/*
	 * If an exception is encountered, processing resumes here.  Report the
	 * error and go away; the postmaster shuts down when we exit with an
	 * error.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* An error from here on is FATAL */
		PG_exception_stack = NULL;

		/* since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		HOLD_INTERRUPTS();

		write_error_file();
		EmitErrorReport();

		/* The exit callbacks release our buffer pins and PGPROC. */
		proc_exit(1);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/* Unblock signals (they were blocked when the postmaster forked us) */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	if (!GetInplaceUpgradeMode())
		elog(ERROR, "in-place upgrade migrator started outside of an in-place upgrade");

	/* The postmaster does not start us once the catalogs are migrated. */
	InplaceUpgradeServerPosture(&posture);
	if (posture.posture == UPGRADE_POSTURE_MIGRATED ||
		posture.posture == UPGRADE_POSTURE_LIVE)
		proc_exit(0);

	if (unlink(INPLACE_UPGRADE_MIGRATE_ERROR) < 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m",
						INPLACE_UPGRADE_MIGRATE_ERROR)));

	migrate_catalogs();

	proc_exit(0);
}

static void
migrate_catalogs(void)
{
	List	   *databases;

	set_ps_display("checking the major version of the catalogs");
	InplaceUpgradeCheckOldMajorVersion();

	/*
	 * Refuse what the migration cannot handle.  Nothing has been written at
	 * this point, so a refusal leaves the data directory as it was.
	 */
	set_ps_display("checking for prepared transactions and replication slots");
	ereport(LOG,
			(errmsg("checking for prepared transactions and replication slots")));
	refuse_prepared_transactions();
	refuse_replication_slots();

	set_ps_display("listing the databases");
	databases = InplaceUpgradeGetDatabases();
	ereport(LOG,
			(errmsg_plural("found %d database to migrate",
						   "found %d databases to migrate",
						   list_length(databases),
						   list_length(databases))));

	set_ps_display("saving files for a rollback");
	save_for_rollback(databases);

	InplaceUpgradeWriteMarker(INPLACE_UPGRADE_MARKER_MIGRATED);
	ereport(LOG,
			(errmsg("the catalogs are migrated")));
}

/*
 * Release our lightweight locks at exit, after an error too.  This is the
 * part of ShutdownPostgres() that applies without a transaction.
 */
static void
migrator_shmem_exit(int code, Datum arg)
{
	LWLockReleaseAll();
	ConditionVariableCancelSleep();
	pgstat_report_wait_end();
}

/*
 * Leave the error being reported where a program driving the upgrade finds
 * it without reading the server log.  Best effort: the error itself is on
 * its way to the server log.
 */
static void
write_error_file(void)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	ErrorData  *edata = CopyErrorData();
	FILE	   *fp;

	MemoryContextSwitchTo(oldcontext);

	fp = AllocateFile(INPLACE_UPGRADE_MIGRATE_ERROR, "w");
	if (fp == NULL)
		return;
	fprintf(fp, "%s\n%s\n", unpack_sql_state(edata->sqlerrcode),
			edata->message);
	FreeFile(fp);
}

/*
 * The state file of a prepared transaction has a format specific to the
 * major version, and the transaction holds locks and row versions that the
 * migration cannot carry over.
 */
static void
refuse_prepared_transactions(void)
{
	int			nprepared = CountPreparedTransactions();

	if (nprepared > 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot migrate the catalogs while prepared transactions exist"),
				 errdetail_plural("There is %d prepared transaction.",
								  "There are %d prepared transactions.",
								  nprepared, nprepared),
				 errhint("Commit or roll back all prepared transactions before shutting down the old major version.")));
}

/*
 * A replication slot holds positions in the WAL of the old major version,
 * and a logical slot also depends on its catalogs.
 */
static void
refuse_replication_slots(void)
{
	int			nslots = 0;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (int i = 0; i < max_replication_slots + max_repack_replication_slots; i++)
	{
		if (ReplicationSlotCtl->replication_slots[i].in_use)
			nslots++;
	}
	LWLockRelease(ReplicationSlotControlLock);

	if (nslots > 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot migrate the catalogs while replication slots exist"),
				 errdetail_plural("There is %d replication slot.",
								  "There are %d replication slots.",
								  nslots, nslots),
				 errhint("Drop all replication slots before shutting down the old major version.")));
}

/*
 * Save what a rollback needs, before the migration changes any of it.
 */
static void
save_for_rollback(List *databases)
{
	int			ndbs = list_length(databases);
	Oid		   *dboids = palloc_array(Oid, ndbs);
	char	  **dbpaths = palloc_array(char *, ndbs);
	char		path[MAXPGPATH];
	char		stashpath[MAXPGPATH];
	int			i = 0;

	ereport(LOG,
			(errmsg("saving the relation map files for a rollback")));

	snprintf(path, sizeof(path), "global/%s", RELMAPPER_FILENAME);
	InplaceUpgradeStashFile(path,
							INPLACE_UPGRADE_DIR "/" RELMAPPER_FILENAME ".global");
	foreach_ptr(MigrateDatabase, db, databases)
	{
		snprintf(path, sizeof(path), "%s/%s", db->path, RELMAPPER_FILENAME);
		snprintf(stashpath, sizeof(stashpath), "%s/%s.%u",
				 INPLACE_UPGRADE_DIR, RELMAPPER_FILENAME, db->oid);
		InplaceUpgradeStashFile(path, stashpath);

		dboids[i] = db->oid;
		dbpaths[i] = db->path;
		i++;
	}
	InplaceUpgradeWriteDatabaseList(dboids, dbpaths, ndbs);

	/*
	 * WIP: a manifest of the data directory would be taken here, for a
	 * rollback to verify that the restored directory has no side effect
	 * nobody registered.  Here it would show the directory after the new
	 * binary's startup, not the old cluster's.
	 */
}
