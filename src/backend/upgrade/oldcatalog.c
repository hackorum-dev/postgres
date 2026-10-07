/*-------------------------------------------------------------------------
 *
 * oldcatalog.c
 *		Reading the catalogs of the old major version at an in-place upgrade
 *
 * The in-place upgrade migrator reads the old version's catalogs without
 * the relation cache: page by page through the buffer manager, with no
 * snapshot, and with copies of the old version's row layouts (see
 * upgrade/catalog_20.h).  Every catalog tuple therefore has to be frozen;
 * the procedure driving the upgrade freezes the catalogs before the old
 * major version shuts down, and a tuple that is not frozen is an error.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/upgrade/oldcatalog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/transam.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace_d.h"
#include "common/int.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/smgr.h"
#include "upgrade/catalog_20.h"
#include "upgrade/inplace_upgrade.h"
#include "utils/relmapper.h"

static void scan_mapped_catalog(const char *dbpath, Oid dbid, Oid tablespace,
								Oid reloid, const char *relname,
								OldTupleCallback callback, void *arg);
static void collect_database(HeapTuple tuple, void *arg);
static void collect_class(HeapTuple tuple, void *arg);
static int	class_entry_cmp(const void *a, const void *b);


/*
 * The catalogs are read with the row layouts of the major version that
 * wrote them (see upgrade/catalog_20.h), so we have to know which one that
 * was.  Until the upgrade goes live, PG_VERSION still names it.  With more
 * than one supported old version, this would pick a set of layouts instead
 * of insisting on the only one.
 */
void
InplaceUpgradeCheckOldMajorVersion(void)
{
	FILE	   *fp;
	int			old_major;

	fp = AllocateFile("PG_VERSION", "r");
	if (fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", "PG_VERSION")));
	if (fscanf(fp, "%d", &old_major) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("file \"%s\" does not contain valid data", "PG_VERSION")));
	FreeFile(fp);

	/* WIP: needs to be a table-driven range check */
	if (old_major != CATALOG_20_MAJOR_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot migrate the catalogs of major version %d",
						old_major)));

	ereport(LOG,
			(errmsg("reading the catalogs with the row layouts of major version %d",
					old_major)));
}

/*
 * Read a relation of the old major version page by page, and hand every
 * tuple to the callback.  The tuple points into the shared buffer, which
 * stays locked while the callback runs; its t_self is its TID in the old
 * file, and its t_tableOid is not set.
 *
 * The tuples are read with no snapshot, so every tuple has to be frozen.
 * One that is not is skipped, and the scan goes on to count them all for
 * the error raised at the end.
 */
void
InplaceUpgradeScanOldCatalog(RelFileLocator rlocator, const char *relname,
							 OldTupleCallback callback, void *arg)
{
	SMgrRelation smgr;
	BlockNumber nblocks;
	uint64		nunfrozen = 0;
	ItemPointerData first_unfrozen;

	smgr = smgropen(rlocator, INVALID_PROC_NUMBER);
	nblocks = smgrnblocks(smgr, MAIN_FORKNUM);
	smgrclose(smgr);

	for (BlockNumber blkno = 0; blkno < nblocks; blkno++)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber maxoff;

		CHECK_FOR_INTERRUPTS();

		buf = ReadBufferWithoutRelcache(rlocator, MAIN_FORKNUM, blkno,
										RBM_NORMAL, NULL, true);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page) || PageIsEmpty(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		maxoff = PageGetMaxOffsetNumber(page);
		for (OffsetNumber offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			ItemId		itemid = PageGetItemId(page, offnum);
			HeapTupleData tuple;

			if (!ItemIdIsNormal(itemid))
				continue;

			tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
			tuple.t_len = ItemIdGetLength(itemid);
			ItemPointerSet(&tuple.t_self, blkno, offnum);
			tuple.t_tableOid = InvalidOid;

			if (heap_tuple_needs_eventual_freeze(tuple.t_data))
			{
				if (nunfrozen++ == 0)
					first_unfrozen = tuple.t_self;
				continue;
			}

			callback(&tuple, arg);
		}

		UnlockReleaseBuffer(buf);
	}

	if (nunfrozen > 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg_plural("catalog \"%s\" has %" PRIu64 " tuple that is not frozen",
							   "catalog \"%s\" has %" PRIu64 " tuples that are not frozen",
							   nunfrozen,
							   relname, nunfrozen),
				 errdetail("The first is tuple (%u,%u).",
						   ItemPointerGetBlockNumber(&first_unfrozen),
						   ItemPointerGetOffsetNumber(&first_unfrozen)),
				 errhint("Run VACUUM (FREEZE) on the catalogs of every database before shutting down the old major version.")));
}

/*
 * Read a catalog that is in the relation map of the database at dbpath.
 */
static void
scan_mapped_catalog(const char *dbpath, Oid dbid, Oid tablespace, Oid reloid,
					const char *relname, OldTupleCallback callback, void *arg)
{
	RelFileLocator rlocator;

	rlocator.spcOid = tablespace;
	rlocator.dbOid = dbid;
	rlocator.relNumber =
		RelationMapOidToFilenumberForDatabase(unconstify(char *, dbpath),
											  reloid);
	if (!RelFileNumberIsValid(rlocator.relNumber))
		elog(ERROR, "could not find \"%s\" in the relation map of \"%s\"",
			 relname, dbpath);

	InplaceUpgradeScanOldCatalog(rlocator, relname, callback, arg);
}

/*
 * List all databases.  A database that allows no connections has catalogs
 * all the same.
 */
List *
InplaceUpgradeGetDatabases(void)
{
	List	   *databases = NIL;

	scan_mapped_catalog("global", InvalidOid, GLOBALTABLESPACE_OID,
						DatabaseRelationId, "pg_database",
						collect_database, &databases);

	return databases;
}

static void
collect_database(HeapTuple tuple, void *arg)
{
	List	  **databases = arg;
	Form_pg_database_20 dbform;
	MigrateDatabase *db;

	dbform = (Form_pg_database_20) GETSTRUCT(tuple);

	if (dbform->datconnlimit == DATCONNLIMIT_INVALID_DB_20)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot migrate the catalogs while database \"%s\" is invalid",
						NameStr(dbform->datname)),
				 errhint("Drop the invalid database before shutting down the old major version.")));

	db = palloc_object(MigrateDatabase);
	db->oid = dbform->oid;
	db->tablespace = dbform->dattablespace;
	db->name = dbform->datname;

	/*
	 * WIP: databases in tablespaces are not migrated.  GetDatabasePath()
	 * names this server's tablespace version directory, which the old version
	 * never created.
	 */
	db->path = GetDatabasePath(db->oid, db->tablespace);
	*databases = lappend(*databases, db);
}

/*
 * Read what the migration needs of pg_class: the file of every relation
 * with an OID in the range of the catalogs, sorted by OID.
 */
ClassEntry *
InplaceUpgradeReadClasses(const MigrateDatabase *db, int *nclasses)
{
	List	   *entries = NIL;
	ClassEntry *classes;
	int			i = 0;

	scan_mapped_catalog(db->path, db->oid, db->tablespace, RelationRelationId,
						"pg_class", collect_class, &entries);

	*nclasses = list_length(entries);
	classes = palloc_array(ClassEntry, *nclasses);
	foreach_ptr(ClassEntry, entry, entries)
		classes[i++] = *entry;
	qsort(classes, *nclasses, sizeof(ClassEntry), class_entry_cmp);

	return classes;
}

static void
collect_class(HeapTuple tuple, void *arg)
{
	List	  **entries = arg;
	Form_pg_class_20 classform;
	ClassEntry *entry;

	classform = (Form_pg_class_20) GETSTRUCT(tuple);
	if (classform->oid >= FirstNormalObjectId)
		return;

	entry = palloc_object(ClassEntry);
	entry->oid = classform->oid;
	entry->relfilenode = classform->relfilenode;
	*entries = lappend(*entries, entry);
}

/*
 * Find a relation in the result of InplaceUpgradeReadClasses(), or return
 * NULL.
 */
const ClassEntry *
InplaceUpgradeLookupClass(const ClassEntry *classes, int nclasses, Oid reloid)
{
	ClassEntry	key;

	key.oid = reloid;
	return bsearch(&key, classes, nclasses, sizeof(ClassEntry),
				   class_entry_cmp);
}

static int
class_entry_cmp(const void *a, const void *b)
{
	return pg_cmp_u32(((const ClassEntry *) a)->oid,
					  ((const ClassEntry *) b)->oid);
}
