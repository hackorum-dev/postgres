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
 * Then it saves anything needed for rollback (see bundle.c) and writes the
 * marker "planned".  Then it rewrites every catalog of every database into a
 * new file (see rewrite.c), switches each database's relation map to the new
 * files once they are all written, and writes the marker "migrated".  A run
 * that finds "planned" present refuses, because an earlier attempt may have
 * changed the catalogs, and only a rollback undoes that.
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
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xlog.h"
#include "catalog/catalog_relations.h"
#include "catalog/pg_tablespace_d.h"
#include "common/inplace_upgrade_posture.h"
#include "common/int.h"
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

/* The relations of one catalog, to rewrite */
typedef struct MigrateCatalog
{
	const CatalogRelation *cat;
	MigrateRelation heap;
	MigrateRelation toast;
	MigrateRelation toastindex;
	MigrateRelation *indexes;	/* cat->nindexes entries */
} MigrateCatalog;

/* The catalogs of one database, or the shared catalogs, to rewrite */
typedef struct MigratePlan
{
	Oid			dbid;			/* InvalidOid for the shared catalogs */
	Oid			tablespace;
	const char *dbpath;
	MigrateCatalog *catalogs;
	int			ncatalogs;
	FilenodeFixup *fixups;		/* new file numbers of the unmapped ones */
	int			nfixups;
	Oid		   *mapped_oids;	/* new file numbers of the mapped ones */
	RelFileNumber *mapped_filenumbers;
	int			nmapped;
} MigratePlan;

static void migrate_catalogs(void);
static void migrator_shmem_exit(int code, Datum arg);
static void write_error_file(void);
static void refuse_prepared_transactions(void);
static void refuse_replication_slots(void);
static void save_for_rollback(List *databases);
static void migrate_shared_catalogs(void);
static void migrate_database(MigrateDatabase *db);
static void plan_catalogs(MigratePlan *plan, const ClassEntry *classes,
						  int nclasses);
static void plan_relation(MigratePlan *plan, MigrateRelation *rel,
						  Oid reloid, const char *name,
						  const ClassEntry *classes, int nclasses);
static void execute_plan(MigratePlan *plan);
static int	fixup_cmp(const void *a, const void *b);
static RelFileNumber allocate_filenumber(const RelFileLocator *locator);


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
	InplaceUpgradePosture posture;

	set_ps_display("checking the major version of the catalogs");
	InplaceUpgradeCheckOldMajorVersion();

	/*
	 * Once the marker "planned" is there, an earlier attempt may have
	 * switched relation maps and rewritten pg_class.  A new attempt would
	 * read those and convert rows twice, and would take the copies kept for a
	 * rollback again, from changed files.  The error of an earlier attempt is
	 * gone by now, so the posture shows the marker as "migrating".
	 */
	InplaceUpgradeServerPosture(&posture);
	if (posture.posture == UPGRADE_POSTURE_MIGRATING)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("an earlier attempt to migrate the catalogs may have changed them"),
				 errhint("Roll the upgrade back.")));

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

	InplaceUpgradeWriteMarker(INPLACE_UPGRADE_MARKER_PLANNED);
	ereport(LOG,
			(errmsg("the migration of the catalogs is planned")));

	migrate_shared_catalogs();
	foreach_ptr(MigrateDatabase, db, databases)
		migrate_database(db);

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

/*
 * The shared catalogs are all in the shared relation map, so there is no
 * pg_class to consult for them.
 */
static void
migrate_shared_catalogs(void)
{
	MigratePlan plan = {0};

	ereport(LOG,
			(errmsg("migrating the shared catalogs")));

	plan.dbid = InvalidOid;
	plan.tablespace = GLOBALTABLESPACE_OID;
	plan.dbpath = "global";
	plan_catalogs(&plan, NULL, 0);
	execute_plan(&plan);
}

static void
migrate_database(MigrateDatabase *db)
{
	MigratePlan plan = {0};
	ClassEntry *classes;
	int			nclasses;

	ereport(LOG,
			(errmsg("migrating the catalogs of database \"%s\"",
					NameStr(db->name))));

	set_ps_display(psprintf("reading pg_class of database %s",
							NameStr(db->name)));
	classes = InplaceUpgradeReadClasses(db, &nclasses);

	plan.dbid = db->oid;
	plan.tablespace = db->tablespace;
	plan.dbpath = db->path;
	plan_catalogs(&plan, classes, nclasses);
	execute_plan(&plan);
}

/*
 * Find the files of every catalog of this version, with its indexes and
 * toast table, and allocate the new file number of each.  Every file number
 * is known before a catalog is rewritten, since the rewrite of pg_class
 * stores the new numbers of the unmapped relations in their rows.
 *
 * With classes NULL, plan the shared catalogs, which are all mapped.
 */
static void
plan_catalogs(MigratePlan *plan, const ClassEntry *classes, int nclasses)
{
	bool		shared = (classes == NULL);
	int			maxrels;

	plan->ncatalogs = 0;
	plan->catalogs = palloc_array(MigrateCatalog, lengthof(CatalogRelations));
	maxrels = 0;
	for (int i = 0; i < lengthof(CatalogRelations); i++)
		maxrels += 3 + CatalogRelations[i].nindexes;
	plan->nfixups = 0;
	plan->fixups = palloc_array(FilenodeFixup, maxrels);
	plan->nmapped = 0;
	plan->mapped_oids = palloc_array(Oid, maxrels);
	plan->mapped_filenumbers = palloc_array(RelFileNumber, maxrels);

	for (int i = 0; i < lengthof(CatalogRelations); i++)
	{
		const CatalogRelation *cat = &CatalogRelations[i];
		MigrateCatalog *mcat;

		if (cat->shared != shared)
			continue;

		mcat = &plan->catalogs[plan->ncatalogs++];
		mcat->cat = cat;
		plan_relation(plan, &mcat->heap, cat->reloid, cat->name,
					  classes, nclasses);
		mcat->indexes = palloc_array(MigrateRelation, cat->nindexes);
		for (int j = 0; j < cat->nindexes; j++)
			plan_relation(plan, &mcat->indexes[j], cat->indexes[j].indexoid,
						  cat->indexes[j].name, classes, nclasses);
		if (OidIsValid(cat->toastoid))
		{
			plan_relation(plan, &mcat->toast, cat->toastoid,
						  psprintf("pg_toast_%u", cat->reloid),
						  classes, nclasses);
			plan_relation(plan, &mcat->toastindex, cat->toastindexoid,
						  psprintf("pg_toast_%u_index", cat->reloid),
						  classes, nclasses);
		}
		else
			mcat->toast.reloid = InvalidOid;
	}

	qsort(plan->fixups, plan->nfixups, sizeof(FilenodeFixup), fixup_cmp);
}

/*
 * Plan one relation: find its file through pg_class or the relation map,
 * and allocate the new one.
 */
static void
plan_relation(MigratePlan *plan, MigrateRelation *rel, Oid reloid,
			  const char *name, const ClassEntry *classes, int nclasses)
{
	RelFileNumber oldfilenumber = InvalidRelFileNumber;

	rel->reloid = reloid;
	rel->name = name;
	rel->oldlocator.spcOid = plan->tablespace;
	rel->oldlocator.dbOid = plan->dbid;
	rel->newlocator = rel->oldlocator;

	if (classes != NULL)
	{
		const ClassEntry *entry;

		entry = InplaceUpgradeLookupClass(classes, nclasses, reloid);
		if (entry == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("catalog relation \"%s\" (OID %u) not found in \"%s/pg_class\"",
							name, reloid, plan->dbpath)));
		oldfilenumber = entry->relfilenode;
	}
	rel->mapped = !RelFileNumberIsValid(oldfilenumber);
	if (rel->mapped)
	{
		oldfilenumber =
			RelationMapOidToFilenumberForDatabase(unconstify(char *, plan->dbpath),
												  reloid);
		if (!RelFileNumberIsValid(oldfilenumber))
			elog(ERROR, "could not find \"%s\" in the relation map of \"%s\"",
				 name, plan->dbpath);
	}
	rel->oldlocator.relNumber = oldfilenumber;
	rel->newlocator.relNumber = allocate_filenumber(&rel->newlocator);

	if (rel->mapped)
	{
		plan->mapped_oids[plan->nmapped] = reloid;
		plan->mapped_filenumbers[plan->nmapped] = rel->newlocator.relNumber;
		plan->nmapped++;
	}
	else
	{
		plan->fixups[plan->nfixups].reloid = reloid;
		plan->fixups[plan->nfixups].relfilenode = rel->newlocator.relNumber;
		plan->nfixups++;
	}
}

/*
 * Rewrite every planned catalog, then switch the relation map to the new
 * files: the moment the new catalogs become the catalogs.
 */
static void
execute_plan(MigratePlan *plan)
{
	for (int i = 0; i < plan->ncatalogs; i++)
	{
		MigrateCatalog *mcat = &plan->catalogs[i];

		InplaceUpgradeRewriteCatalog(mcat->cat, &mcat->heap, mcat->indexes,
									 OidIsValid(mcat->toast.reloid) ?
									 &mcat->toast : NULL,
									 &mcat->toastindex,
									 plan->fixups, plan->nfixups);
	}

	set_ps_display(psprintf("switching the relation map of %s",
							plan->dbpath));
	RelationMapUpdateFileForDatabase(plan->dbid, plan->tablespace,
									 plan->dbpath, plan->mapped_oids,
									 plan->mapped_filenumbers, plan->nmapped);
}

static int
fixup_cmp(const void *a, const void *b)
{
	return pg_cmp_u32(((const FilenodeFixup *) a)->reloid,
					  ((const FilenodeFixup *) b)->reloid);
}

/*
 * A file number for a new relation file in the database and tablespace of
 * the locator: as GetNewRelFileNumber() does it, but for a database we are
 * not connected to.  The OID counter is WAL-logged, so the standbys will
 * have the same counter after the upgrade.
 */
static RelFileNumber
allocate_filenumber(const RelFileLocator *locator)
{
	RelFileLocator candidate = *locator;
	bool		collides;

	do
	{
		SMgrRelation srel;

		CHECK_FOR_INTERRUPTS();

		candidate.relNumber = GetNewObjectId();

		/* Check for an existing file of the same name */
		srel = smgropen(candidate, INVALID_PROC_NUMBER);
		collides = smgrexists(srel, MAIN_FORKNUM);
		smgrclose(srel);
	} while (collides);

	return candidate.relNumber;
}
