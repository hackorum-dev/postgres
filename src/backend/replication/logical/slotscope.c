/*-------------------------------------------------------------------------
 *
 * slotscope.c
 *	  Publication-backed relation scopes for restricted logical slots.
 *
 * A restricted logical replication slot writes logical tuple WAL only for
 * relations that may be needed by the slot's publications.  The slot stores
 * its immutable publication OIDs in a sidecar file, while
 * pg_restricted_slot_relation maintains the current physical mappings from
 * slot incarnations to relations.  pg_class.relhasrestrictedslots provides
 * the fast relation-level decision used during WAL insertion.
 *
 * The writer-side mapping is deliberately conservative.  Stale mappings may
 * cause unnecessary logical WAL to be written until they are cleaned up, but
 * a required mapping must never be missing.  This mapping does not determine
 * which changes are emitted by an output plugin; pgoutput continues to apply
 * the current publication definitions when decoding.
 *
 * Publication expansion, schema membership, partition attachment, and TOAST
 * creation update the mappings transactionally.  Obsolete mappings are
 * removed lazily after their slot name and incarnation no longer identify a
 * live restricted slot.
 *
 * Slot files and catalog mappings cannot be persisted atomically.  Restricted
 * slot creation therefore temporarily enables full logical WAL, installs the
 * mappings and a completion marker in a transaction, and marks the slot ready
 * after that transaction commits.  After a restart, incomplete slot state is
 * reconciled using the durable completion marker.
 *
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/slotscope.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_restricted_slot_relation.h"
#include "common/file_utils.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "replication/logicalctl.h"
#include "replication/slot.h"
#include "replication/slotscope.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "utils/fmgroids.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#define SLOT_PUBLICATIONS_MAGIC 0x51C0B11EU
#define SLOT_PUBLICATIONS_VERSION 1
#define SLOT_PUBLICATIONS_FILE "publications"
#define SLOT_SCOPE_CHANGE_LOCK_SUBID 1

typedef struct LogicalSlotPublicationsOnDisk
{
	uint32		magic;
	pg_crc32c	checksum;
	uint32		version;
	uint32		npublications;
	Oid			publications[FLEXIBLE_ARRAY_MEMBER];
}			LogicalSlotPublicationsOnDisk;

static List *read_publications_file(ReplicationSlot *slot);
static List *publication_relation_closure(List *publications,
										  bool conditional);
static List *restricted_slots_for_publication(Oid pubid);
static void restricted_relation_add(const char *slotname, Oid relid,
									bool conditional);
static void restricted_ready_marker_add(const char *slotname,
										uint64 incarnation);
static List *pending_ready_slots;
static bool callbacks_registered;

typedef struct PendingReadySlot
{
	ReplicationSlot *slot;
	NameData	name;
	bool		finalize_at_xact_end;
}			PendingReadySlot;

typedef struct ReconcileSlot
{
	NameData	name;
	uint64		incarnation;
}			ReconcileSlot;

/*
 * Acquire the database-wide lock that serializes restricted-slot scope
 * operations.  DDL paths release this lock explicitly after updating scope
 * mappings, while initialization and maintenance may retain it until
 * transaction end.
 */
static void
lock_database_scope(void)
{
	LockDatabaseObject(RestrictedSlotRelationRelationId, MyDatabaseId, 0,
					   ExclusiveLock);
}

/*
 * Release a database scope lock acquired for a single DDL scope operation.
 * The transaction-level scope-change marker, if any, remains held until
 * transaction end.
 */
static void
unlock_database_scope(void)
{
	UnlockDatabaseObject(RestrictedSlotRelationRelationId, MyDatabaseId, 0,
						 ExclusiveLock);
}

/*
 * Mark the current transaction as containing an uncommitted scope-changing
 * DDL operation.
 *
 * DDL transactions acquire this marker in ShareLock mode and retain it until
 * transaction end.  Restricted-slot initialization conditionally requests an
 * ExclusiveLock on the same lock tag, preventing it from constructing an
 * initial scope while relevant catalog changes remain uncommitted.
 */
static void
mark_database_scope_change(void)
{
	LockDatabaseObject(RestrictedSlotRelationRelationId, MyDatabaseId,
					   SLOT_SCOPE_CHANGE_LOCK_SUBID, ShareLock);
}

/*
 * Ensure that no other transaction has an uncommitted scope-changing DDL
 * operation.
 *
 * Acquire the transaction-level scope-change marker conditionally in
 * ExclusiveLock mode.  Fail with a retryable error instead of waiting, since
 * the conflicting DDL may already hold relation locks needed by slot
 * initialization.
 */
static void
check_no_database_scope_change(void)
{
	if (!ConditionalLockDatabaseObject(RestrictedSlotRelationRelationId,
									   MyDatabaseId,
									   SLOT_SCOPE_CHANGE_LOCK_SUBID,
									   ExclusiveLock))
		ereport(ERROR,
				(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
				 errmsg("could not initialize restricted logical replication slot due to concurrent activity"),
				 errhint("Retry creating the replication slot.")));
}

/*
 * Acquire ShareRowExclusiveLock on a relation while constructing or updating
 * a restricted slot's physical scope.
 *
 * When conditional is true, fail with a retryable error rather than waiting.
 * Slot initialization uses conditional locking because it already holds the
 * database scope lock and waiting for DDL-held relation locks could deadlock.
 * DDL paths use normal blocking locks before acquiring the scope lock.
 */
static void
lock_scope_relation(Oid relid, bool conditional)
{
	if (conditional)
	{
		if (ConditionalLockRelationOid(relid, ShareRowExclusiveLock))
			return;
		ereport(ERROR,
				(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
				 errmsg("could not initialize restricted logical replication slot due to concurrent activity"),
				 errhint("Retry creating the replication slot.")));
	}
	LockRelationOid(relid, ShareRowExclusiveLock);
}

/*
 * Complete pending restricted-slot initializations at transaction end.
 *
 * The initial relation mappings and readiness marker are transactional, but
 * replication-slot state is not.  After commit, mark each pending slot ready
 * at the end of the commit record and optionally release a slot whose caller
 * delegated finalization to this callback.  Request asynchronous removal of
 * the temporary full-WAL requirement once the restricted mappings are
 * committed.
 *
 * On abort, release slots delegated to this callback as ephemeral so that
 * their incomplete on-disk state is removed.  Other callers retain
 * responsibility for releasing their slots.
 */
static void
slot_scope_xact_callback(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT)
	{
		foreach_ptr(PendingReadySlot, pending, pending_ready_slots)
		{
			ReplicationSlot *slot = pending->slot;

			if (slot->in_use &&
				strcmp(NameStr(slot->data.name), NameStr(pending->name)) == 0)
			{
				SpinLockAcquire(&slot->mutex);
				slot->data.restricted_scope_ready = true;
				slot->data.restricted_scope_ready_lsn = XactLastCommitEnd;
				slot->just_dirtied = true;
				slot->dirty = true;
				SpinLockRelease(&slot->mutex);
				if (pending->finalize_at_xact_end && MyReplicationSlot == slot)
					ReplicationSlotRelease();
			}
		}
		if (pending_ready_slots != NIL)
			RequestDisableLogicalDecoding();
	}
	else if (event == XACT_EVENT_ABORT)
	{
		foreach_ptr(PendingReadySlot, pending, pending_ready_slots)
		{
			ReplicationSlot *slot = pending->slot;

			if (!pending->finalize_at_xact_end || MyReplicationSlot != slot)
				continue;
			SpinLockAcquire(&slot->mutex);
			slot->data.persistency = RS_EPHEMERAL;
			SpinLockRelease(&slot->mutex);
			ReplicationSlotRelease();
		}
	}
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT)
	{
		list_free_deep(pending_ready_slots);
		pending_ready_slots = NIL;
	}
}

/* Register the transaction callback used by restricted-slot initialization. */
void
LogicalSlotScopeInitialize(void)
{
	if (!callbacks_registered)
	{
		RegisterXactCallback(slot_scope_xact_callback, NULL);
		callbacks_registered = true;
	}
}

/*
 * Atomically replace a restricted slot's publication side file.
 *
 * The file stores the immutable publication identities associated with the
 * slot.  Write and fsync a temporary file, then durably rename it over the
 * previous file.  The slot I/O lock serializes this operation with readers
 * and other slot-file operations.
 */
static void
write_publications_file(ReplicationSlot *slot, List *publications)
{
	LogicalSlotPublicationsOnDisk *ondisk;
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];
	Size		size;
	int			fd;
	int			i = 0;

	size = offsetof(LogicalSlotPublicationsOnDisk, publications) +
		list_length(publications) * sizeof(Oid);
	ondisk = palloc0(size);
	ondisk->magic = SLOT_PUBLICATIONS_MAGIC;
	ondisk->version = SLOT_PUBLICATIONS_VERSION;
	ondisk->npublications = list_length(publications);
	foreach_oid(pubid, publications)
		ondisk->publications[i++] = pubid;
	INIT_CRC32C(ondisk->checksum);
	COMP_CRC32C(ondisk->checksum,
				(char *) ondisk + offsetof(LogicalSlotPublicationsOnDisk, version),
				size - offsetof(LogicalSlotPublicationsOnDisk, version));
	FIN_CRC32C(ondisk->checksum);

	snprintf(path, sizeof(path), "%s/%s/%s", PG_REPLSLOT_DIR,
			 NameStr(slot->data.name), SLOT_PUBLICATIONS_FILE);
	snprintf(tmppath, sizeof(tmppath), "%s.tmp", path);
	LWLockAcquire(&slot->io_in_progress_lock, LW_EXCLUSIVE);
	if (unlink(tmppath) < 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", tmppath)));
	fd = OpenTransientFile(tmppath, O_CREAT | O_EXCL | O_WRONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmppath)));
	if (write(fd, ondisk, size) != size || pg_fsync(fd) != 0 ||
		CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", tmppath)));
	(void) durable_rename(tmppath, path, ERROR);
	LWLockRelease(&slot->io_in_progress_lock);
	pfree(ondisk);
}

/*
 * Read and validate a restricted slot's publication side file.
 *
 * Verify the file header, size, version, and checksum before returning its
 * publication OIDs.  The slot I/O lock prevents the file from being replaced
 * while it is being read.  Unrestricted slots have no publication file and
 * return an empty list.
 */
static List *
read_publications_file(ReplicationSlot *slot)
{
	LogicalSlotPublicationsOnDisk *ondisk;
	pg_crc32c	checksum;
	struct stat st;
	char		path[MAXPGPATH];
	List	   *result = NIL;
	Size		expected;
	int			fd;

	if (slot->data.unrestricted)
		return NIL;
	snprintf(path, sizeof(path), "%s/%s/%s", PG_REPLSLOT_DIR,
			 NameStr(slot->data.name), SLOT_PUBLICATIONS_FILE);
	LWLockAcquire(&slot->io_in_progress_lock, LW_SHARED);
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0 || fstat(fd, &st) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open restricted slot publication file \"%s\": %m", path)));
	if (st.st_size < offsetof(LogicalSlotPublicationsOnDisk, publications))
		ereport(ERROR,
				(errmsg("restricted slot publication file \"%s\" is too small", path)));
	ondisk = palloc(st.st_size);
	if (read(fd, ondisk, st.st_size) != st.st_size ||
		CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read restricted slot publication file \"%s\": %m", path)));
	LWLockRelease(&slot->io_in_progress_lock);
	expected = offsetof(LogicalSlotPublicationsOnDisk, publications) +
		ondisk->npublications * sizeof(Oid);
	if (ondisk->magic != SLOT_PUBLICATIONS_MAGIC ||
		ondisk->version != SLOT_PUBLICATIONS_VERSION || expected != st.st_size)
		ereport(ERROR,
				(errmsg("invalid restricted slot publication file \"%s\"", path)));
	INIT_CRC32C(checksum);
	COMP_CRC32C(checksum,
				(char *) ondisk + offsetof(LogicalSlotPublicationsOnDisk, version),
				st.st_size - offsetof(LogicalSlotPublicationsOnDisk, version));
	FIN_CRC32C(checksum);
	if (!EQ_CRC32C(checksum, ondisk->checksum))
		ereport(ERROR,
				(errmsg("checksum mismatch for restricted slot publication file \"%s\"", path)));
	for (uint32 i = 0; i < ondisk->npublications; i++)
		result = lappend_oid(result, ondisk->publications[i]);
	pfree(ondisk);
	return result;
}

/* Return the publication OIDs stored for a restricted slot. */
List *
LogicalSlotScopeGetPublications(ReplicationSlot *slot)
{
	return read_publications_file(slot);
}

/*
 * Build and lock the current physical relation closure of a publication set.
 *
 * Expand explicit and schema publication members to include partition
 * descendants, then add each relation's current TOAST relation.  Ignore
 * publications that have been dropped, reject FOR ALL TABLES publications,
 * and remove duplicate relation OIDs.
 *
 * Acquire ShareRowExclusiveLock on every base relation so no writer can cross
 * scope activation using stale relation metadata.  When conditional is true,
 * fail instead of waiting for a conflicting relation lock; this mode is used
 * during restricted-slot initialization to avoid deadlocks with concurrent
 * DDL.
 */
static List *
publication_relation_closure(List *publications, bool conditional)
{
	List	   *relations = NIL;
	List	   *base;

	foreach_oid(pubid, publications)
	{
		Publication *pub;
		List	   *pubrels;

		if (!SearchSysCacheExists1(PUBLICATIONOID, ObjectIdGetDatum(pubid)))
			continue;
		pub = GetPublication(pubid);
		if (pub->alltables)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("publication \"%s\" is defined FOR ALL TABLES", pub->name),
					 errhint("Create an unrestricted logical replication slot instead.")));
		pubrels = GetIncludedPublicationRelations(pubid, PUBLICATION_PART_ALL);
		pubrels = list_concat(pubrels,
							  GetAllSchemaPublicationRelations(pubid, PUBLICATION_PART_ALL));
		relations = list_concat(relations, pubrels);
	}
	list_sort(relations, list_oid_cmp);
	list_deduplicate_oid(relations);

	base = list_copy(relations);
	foreach_oid(relid, base)
	{
		Relation	rel;

		lock_scope_relation(relid, conditional);
		rel = table_open(relid, NoLock);

		if (OidIsValid(rel->rd_rel->reltoastrelid))
			relations = lappend_oid(relations, rel->rd_rel->reltoastrelid);
		table_close(rel, NoLock);
	}
	list_free(base);

	list_sort(relations, list_oid_cmp);
	list_deduplicate_oid(relations);
	return relations;
}

/* Set pg_class.relhasrestrictedslots for a mapped relation. */
static void
set_relation_restricted_flag(Oid relid)
{
	Relation	classrel;
	HeapTuple	tuple;
	Form_pg_class classform;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation with OID %u does not exist", relid)));

	if (((Form_pg_class) GETSTRUCT(tuple))->relhasrestrictedslots)
	{
		ReleaseSysCache(tuple);
		return;
	}
	ReleaseSysCache(tuple);

	/*
	 * Fetch and check the tuple again while holding the relation lock
	 * acquired by restricted_relation_add(), because its value might have
	 * changed since the fast-path check.
	 */
	classrel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation with OID %u does not exist", relid)));

	classform = (Form_pg_class) GETSTRUCT(tuple);
	if (!classform->relhasrestrictedslots)
	{
		classform->relhasrestrictedslots = true;
		CatalogTupleUpdate(classrel, &tuple->t_self, tuple);
		CommandCounterIncrement();
	}

	heap_freetuple(tuple);
	table_close(classrel, RowExclusiveLock);
}

/* Look up the current incarnation of a named logical replication slot. */
static uint64
restricted_slot_incarnation(const char *slotname)
{
	uint64		incarnation = 0;

	LWLockAcquire(ReplicationSlotAllocationLock, LW_SHARED);
	for (int i = 0; i < max_replication_slots + max_repack_replication_slots; i++)
	{
		ReplicationSlot *slot = &ReplicationSlotCtl->replication_slots[i];
		ReplicationSlotPersistentData data;

		if (!slot->in_use)
			continue;
		SpinLockAcquire(&slot->mutex);
		data = slot->data;
		SpinLockRelease(&slot->mutex);
		if (data.database != InvalidOid &&
			strcmp(NameStr(data.name), slotname) == 0)
		{
			incarnation = data.restricted_scope_incarnation;
			break;
		}
	}
	LWLockRelease(ReplicationSlotAllocationLock);
	return incarnation;
}

/*
 * Add a relation mapping for the current incarnation of a restricted slot.
 *
 * If the named slot no longer exists, do nothing.  Otherwise, lock the
 * relation against concurrent DML, insert the incarnation-qualified mapping
 * if it is absent, and set pg_class.relhasrestrictedslots.
 *
 * The relation lock prevents a writer from crossing transaction commit with
 * a stale false value for relhasrestrictedslots.  The database-object lock
 * serializes mapping changes with cleanup and other scope maintenance.
 */
static void
restricted_relation_add(const char *slotname, Oid relid, bool conditional)
{
	Relation	maprel;
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	NameData	keyname;
	uint64		incarnation = restricted_slot_incarnation(slotname);

	if (incarnation == 0)
		return;

	/* Keep DML out until the new flag and mapping become visible at commit. */
	lock_scope_relation(relid, conditional);
	maprel = table_open(RestrictedSlotRelationRelationId, RowExclusiveLock);
	namestrcpy(&keyname, slotname);
	ScanKeyInit(&keys[0], Anum_pg_restricted_slot_relation_rsrslotname,
				BTEqualStrategyNumber, F_NAMEEQ, NameGetDatum(&keyname));
	ScanKeyInit(&keys[1], Anum_pg_restricted_slot_relation_rsrrelid,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
	ScanKeyInit(&keys[2], Anum_pg_restricted_slot_relation_rsrincarnation,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64) incarnation));
	scan = systable_beginscan(maprel, RestrictedSlotRelationSlotRelIndexId,
							  true, NULL, 3, keys);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
	{
		Datum		values[Natts_pg_restricted_slot_relation];
		bool		nulls[Natts_pg_restricted_slot_relation] = {false};
		NameData	name;

		MemSet(values, 0, sizeof(values));
		namestrcpy(&name, slotname);
		values[Anum_pg_restricted_slot_relation_rsrslotname - 1] = NameGetDatum(&name);
		values[Anum_pg_restricted_slot_relation_rsrrelid - 1] = ObjectIdGetDatum(relid);
		values[Anum_pg_restricted_slot_relation_rsrincarnation - 1] =
			Int64GetDatum((int64) incarnation);
		tuple = heap_form_tuple(RelationGetDescr(maprel), values, nulls);
		CatalogTupleInsert(maprel, tuple);
		heap_freetuple(tuple);
		CommandCounterIncrement();
	}
	systable_endscan(scan);
	table_close(maprel, RowExclusiveLock);
	set_relation_restricted_flag(relid);
}

/*
 * Add the transactional readiness marker for a restricted-slot incarnation.
 *
 * The marker uses InvalidOid as its relation OID and is inserted in the same
 * transaction as the initial relation mappings.  After a crash, its presence
 * proves that initialization committed, including when the publications had
 * no relation members.  Its absence means that a not-ready slot must be
 * treated as incomplete.
 */
static void
restricted_ready_marker_add(const char *slotname, uint64 incarnation)
{
	Relation	maprel;
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	NameData	keyname;

	maprel = table_open(RestrictedSlotRelationRelationId, RowExclusiveLock);
	namestrcpy(&keyname, slotname);
	ScanKeyInit(&keys[0], Anum_pg_restricted_slot_relation_rsrslotname,
				BTEqualStrategyNumber, F_NAMEEQ, NameGetDatum(&keyname));
	ScanKeyInit(&keys[1], Anum_pg_restricted_slot_relation_rsrrelid,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&keys[2], Anum_pg_restricted_slot_relation_rsrincarnation,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64) incarnation));
	scan = systable_beginscan(maprel, RestrictedSlotRelationSlotRelIndexId,
							  true, NULL, 3, keys);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
	{
		Datum		values[Natts_pg_restricted_slot_relation];
		bool		nulls[Natts_pg_restricted_slot_relation] = {false};
		NameData	name;

		MemSet(values, 0, sizeof(values));
		namestrcpy(&name, slotname);
		values[Anum_pg_restricted_slot_relation_rsrslotname - 1] =
			NameGetDatum(&name);
		values[Anum_pg_restricted_slot_relation_rsrrelid - 1] =
			ObjectIdGetDatum(InvalidOid);
		values[Anum_pg_restricted_slot_relation_rsrincarnation - 1] =
			Int64GetDatum((int64) incarnation);
		tuple = heap_form_tuple(RelationGetDescr(maprel), values, nulls);
		CatalogTupleInsert(maprel, tuple);
		heap_freetuple(tuple);
	}
	systable_endscan(scan);
	table_close(maprel, RowExclusiveLock);
}

/* Return the names of restricted slots currently mapped to a relation. */
static List *
restricted_slots_for_relation(Oid relid)
{
	Relation	maprel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	maprel = table_open(RestrictedSlotRelationRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_restricted_slot_relation_rsrrelid,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(maprel, RestrictedSlotRelationRelIndexId,
							  true, NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_restricted_slot_relation form =
			(Form_pg_restricted_slot_relation) GETSTRUCT(tuple);

		result = lappend(result, pstrdup(NameStr(form->rsrslotname)));
	}
	systable_endscan(scan);
	table_close(maprel, AccessShareLock);
	return result;
}

/*
 * Return the names of valid restricted slots in the current database whose
 * stored publication set contains pubid.
 *
 * Copy the slot names while holding the replication-slot allocation lock, but
 * do not retain that lock across relation locking or mapping catalog changes.
 * Include not-yet-ready restricted slots so concurrent publication expansion
 * cannot be missed during slot initialization.
 */
static List *
restricted_slots_for_publication(Oid pubid)
{
	List	   *slotnames = NIL;

	LWLockAcquire(ReplicationSlotAllocationLock, LW_SHARED);
	for (int i = 0; i < max_replication_slots + max_repack_replication_slots; i++)
	{
		ReplicationSlot *slot = &ReplicationSlotCtl->replication_slots[i];
		ReplicationSlotPersistentData data;
		List	   *publications;

		if (!slot->in_use)
			continue;
		SpinLockAcquire(&slot->mutex);
		data = slot->data;
		SpinLockRelease(&slot->mutex);
		if (data.database != MyDatabaseId || data.unrestricted ||
			data.invalidated != RS_INVAL_NONE)
			continue;
		publications = read_publications_file(slot);
		if (list_member_oid(publications, pubid))
			slotnames = lappend(slotnames, pstrdup(NameStr(data.name)));
		list_free(publications);
	}
	LWLockRelease(ReplicationSlotAllocationLock);
	return slotnames;
}

/*
 * Build and lock the current physical closure of a list of relation roots.
 *
 * Discover every root and partition descendant under AccessShareLock, add
 * their current TOAST relations, and sort and deduplicate the resulting OIDs.
 * Acquire ShareRowExclusiveLock in OID order to prevent concurrent writers
 * from crossing the transactional installation of scope mappings and to
 * avoid inconsistent relation-lock ordering between concurrent operations.
 */
static List *
relation_physical_closure(List *roots)
{
	List	   *result = NIL;
	List	   *base;
	List	   *sorted_roots = list_copy(roots);

	list_sort(sorted_roots, list_oid_cmp);
	list_deduplicate_oid(sorted_roots);
	foreach_oid(root, sorted_roots)
	{
		List	   *descendants = find_all_inheritors(root,
													  AccessShareLock, NULL);

		result = list_concat(result, descendants);
	}
	list_free(sorted_roots);
	list_sort(result, list_oid_cmp);
	list_deduplicate_oid(result);

	base = list_copy(result);
	foreach_oid(relid, base)
	{
		Relation	rel = table_open(relid, NoLock);

		if (OidIsValid(rel->rd_rel->reltoastrelid))
			result = lappend_oid(result, rel->rd_rel->reltoastrelid);
		table_close(rel, NoLock);
	}
	list_free(base);

	list_sort(result, list_oid_cmp);
	list_deduplicate_oid(result);
	foreach_oid(relid, result)
		LockRelationOid(relid, ShareRowExclusiveLock);
	return result;
}

/* Add physical relations to a named restricted slot's scope. */
static void
logical_slot_scope_add_relations(const char *slotname, List *relations,
								 bool conditional)
{
	foreach_oid(relid, relations)
		restricted_relation_add(slotname, relid, conditional);
}

/*
 * Add newly published relations to restricted slots using a publication.
 *
 * Find valid restricted slots in the current database whose stored
 * publication set contains pubid.  Expand the supplied relation roots to
 * their partition descendants and TOAST relations, then add the resulting
 * mappings to each matching slot.
 *
 * The mappings and pg_class flags are changed in the caller's transaction,
 * so they become visible atomically with the publication membership change.
 * Relation locks prevent concurrent DML from missing logical tuple WAL while
 * that change is being committed.
 */
void
LogicalSlotScopePublicationAddRelations(Oid pubid, List *relations)
{
	List	   *slotnames;
	List	   *closure;

	/*
	 * To avoid unconditionally doing relation_physical_closure, check if
	 * there are any restricted slots for the publication first.  If not, we
	 * can skip the closure and locking entirely.  This is important for
	 * performance, as relation_physical_closure can be expensive for large
	 * relation sets.
	 */
	mark_database_scope_change();
	lock_database_scope();
	slotnames = restricted_slots_for_publication(pubid);
	unlock_database_scope();
	if (slotnames == NIL)
		return;
	list_free_deep(slotnames);

	closure = relation_physical_closure(relations);
	lock_database_scope();
	/* Recheck after taking relation locks and reacquiring serialization. */
	slotnames = restricted_slots_for_publication(pubid);
	if (slotnames == NIL)
	{
		list_free(closure);
		unlock_database_scope();
		return;
	}
	EnsureRestrictedLogicalWAL();
	foreach_ptr(char, slotname, slotnames)
		logical_slot_scope_add_relations(slotname, closure, false);
	list_free(closure);
	list_free_deep(slotnames);
	unlock_database_scope();
}

/*
 * Propagate an owner's restricted-slot mappings to a new TOAST relation.
 *
 * Add the TOAST relation to every restricted slot currently mapped to its
 * owning relation.  The mapping and relhasrestrictedslots flag are changed
 * in the transaction that creates the TOAST relation, so subsequent TOAST
 * writes cannot become visible without the required logical WAL.
 */
void
LogicalSlotScopeNoteToastCreation(Oid owner, Oid toastrelid)
{
	List	   *slotnames;

	if (IsBootstrapProcessingMode())
		return;
	mark_database_scope_change();
	lock_database_scope();
	slotnames = restricted_slots_for_relation(owner);
	foreach_ptr(char, slotname, slotnames)
		restricted_relation_add(slotname, toastrelid, false);
	list_free_deep(slotnames);
	unlock_database_scope();
}

/*
 * Remove all restricted-slot mappings for a relation being dropped.
 *
 * Delete mappings for every slot incarnation in the same transaction that
 * drops the relation.  There is no need to clear relhasrestrictedslots
 * because the relation's pg_class row is also being removed.
 */
void
LogicalSlotScopeRelationDrop(Oid relid)
{
	Relation	maprel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;

	mark_database_scope_change();
	lock_database_scope();
	maprel = table_open(RestrictedSlotRelationRelationId, RowExclusiveLock);
	ScanKeyInit(&key, Anum_pg_restricted_slot_relation_rsrrelid,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(maprel, RestrictedSlotRelationRelIndexId,
							  true, NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		CatalogTupleDelete(maprel, &tuple->t_self);
	systable_endscan(scan);
	table_close(maprel, RowExclusiveLock);
	unlock_database_scope();
}

/* Return whether any restricted-slot mapping remains for a relation. */
static bool
relation_has_mapping(Oid relid)
{
	Relation	maprel;
	ScanKeyData key;
	SysScanDesc scan;
	bool		result;

	maprel = table_open(RestrictedSlotRelationRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_restricted_slot_relation_rsrrelid,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(maprel, RestrictedSlotRelationRelIndexId,
							  true, NULL, 1, &key);
	result = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(maprel, AccessShareLock);
	return result;
}

/* Check for a committed readiness marker matching a slot incarnation. */
static bool
restricted_ready_marker_exists(const char *slotname, uint64 incarnation)
{
	Relation	maprel;
	ScanKeyData keys[3];
	SysScanDesc scan;
	NameData	name;
	bool		result;

	maprel = table_open(RestrictedSlotRelationRelationId, AccessShareLock);
	namestrcpy(&name, slotname);
	ScanKeyInit(&keys[0], Anum_pg_restricted_slot_relation_rsrslotname,
				BTEqualStrategyNumber, F_NAMEEQ, NameGetDatum(&name));
	ScanKeyInit(&keys[1], Anum_pg_restricted_slot_relation_rsrrelid,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&keys[2], Anum_pg_restricted_slot_relation_rsrincarnation,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64) incarnation));
	scan = systable_beginscan(maprel, RestrictedSlotRelationSlotRelIndexId,
							  true, NULL, 3, keys);
	result = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(maprel, AccessShareLock);
	return result;
}

/*
 * Reconcile incomplete restricted slots for the current database.
 *
 * Slot state and transactional relation mappings cannot be persisted
 * atomically.  After a crash, a slot may therefore remain not ready even
 * though its mapping transaction committed.
 *
 * For each inactive, valid restricted slot that is not ready, look for the
 * readiness marker matching its name and incarnation.  If the marker exists,
 * mark the slot ready and use the current flush position as a conservative
 * mapping-durability boundary.  If the marker does not exist, drop the slot
 * as an aborted or incomplete creation.
 *
 * Slot state is rechecked after acquiring each slot because it may have
 * changed since the initial scan.
 */
void
LogicalSlotScopeReconcileDatabase(void)
{
	List	   *slots = NIL;
	bool		found = false;

	Assert(IsTransactionState());

	/* Avoid catalog locking on connections with nothing to reconcile. */
	LWLockAcquire(ReplicationSlotAllocationLock, LW_SHARED);
	for (int i = 0; i < max_replication_slots + max_repack_replication_slots; i++)
	{
		ReplicationSlot *slot = &ReplicationSlotCtl->replication_slots[i];
		ReplicationSlotPersistentData data;
		bool		active;

		if (!slot->in_use)
			continue;
		SpinLockAcquire(&slot->mutex);
		data = slot->data;
		active = slot->active_proc != INVALID_PROC_NUMBER;
		SpinLockRelease(&slot->mutex);
		if (data.database != MyDatabaseId || data.unrestricted ||
			data.restricted_scope_ready ||
			data.invalidated != RS_INVAL_NONE || active)
			continue;
		found = true;
		break;
	}
	LWLockRelease(ReplicationSlotAllocationLock);
	if (!found)
		return;

	lock_database_scope();
	LWLockAcquire(ReplicationSlotAllocationLock, LW_SHARED);
	for (int i = 0; i < max_replication_slots + max_repack_replication_slots; i++)
	{
		ReplicationSlot *slot = &ReplicationSlotCtl->replication_slots[i];
		ReplicationSlotPersistentData data;
		ReconcileSlot *item;
		bool		active;

		if (!slot->in_use)
			continue;
		SpinLockAcquire(&slot->mutex);
		data = slot->data;
		active = slot->active_proc != INVALID_PROC_NUMBER;
		SpinLockRelease(&slot->mutex);
		if (data.database != MyDatabaseId || data.unrestricted ||
			data.restricted_scope_ready ||
			data.invalidated != RS_INVAL_NONE || active)
			continue;
		item = palloc(sizeof(*item));
		item->name = data.name;
		item->incarnation = data.restricted_scope_incarnation;
		slots = lappend(slots, item);
	}
	LWLockRelease(ReplicationSlotAllocationLock);

	foreach_ptr(ReconcileSlot, item, slots)
	{
		if (restricted_ready_marker_exists(NameStr(item->name), item->incarnation))
		{
			bool		reconciled = false;
			ReplicationSlotPersistentData data;

			if (!ReplicationSlotConditionalAcquire(NameStr(item->name), false))
				continue;
			SpinLockAcquire(&MyReplicationSlot->mutex);
			data = MyReplicationSlot->data;
			if (data.database == MyDatabaseId && !data.unrestricted &&
				!data.restricted_scope_ready &&
				data.invalidated == RS_INVAL_NONE &&
				data.restricted_scope_incarnation == item->incarnation)
			{
				MyReplicationSlot->data.restricted_scope_ready = true;
				MyReplicationSlot->data.restricted_scope_ready_lsn = GetFlushRecPtr(NULL);
				MyReplicationSlot->just_dirtied = true;
				MyReplicationSlot->dirty = true;
				reconciled = true;
			}
			SpinLockRelease(&MyReplicationSlot->mutex);
			ReplicationSlotRelease();
			if (reconciled)
			{
				EnableLogicalDecoding();
				EnsureRestrictedLogicalWAL();
				RequestDisableLogicalDecoding();
			}
		}
		else
		{
			ReplicationSlotPersistentData data;

			if (!ReplicationSlotConditionalAcquire(NameStr(item->name), false))
				continue;
			SpinLockAcquire(&MyReplicationSlot->mutex);
			data = MyReplicationSlot->data;
			SpinLockRelease(&MyReplicationSlot->mutex);
			if (data.database == MyDatabaseId && !data.unrestricted &&
				!data.restricted_scope_ready &&
				data.restricted_scope_incarnation == item->incarnation)
				ReplicationSlotDropAcquired(true);
			else
				ReplicationSlotRelease();
		}
	}
	list_free_deep(slots);
}

/* Test whether a list contains a matching slot name and incarnation. */
static bool
slot_identity_list_contains(List *slots, const char *slotname,
							uint64 incarnation)
{
	foreach_ptr(ReconcileSlot, candidate, slots)
		if (strcmp(NameStr(candidate->name), slotname) == 0 &&
			candidate->incarnation == incarnation)
		return true;
	return false;
}

/* Clear relhasrestrictedslots after a relation loses its last mapping. */
static void
clear_relation_restricted_flag(Oid relid)
{
	Relation	classrel;
	HeapTuple	tuple;

	/* A stale true flag is safe; maintenance must not wait behind DDL. */
	if (!ConditionalLockRelationOid(relid, ShareRowExclusiveLock))
		return;
	if (relation_has_mapping(relid))
		return;
	classrel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tuple))
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tuple);

		if (form->relhasrestrictedslots)
		{
			form->relhasrestrictedslots = false;
			CatalogTupleUpdate(classrel, &tuple->t_self, tuple);
		}
		heap_freetuple(tuple);
	}
	table_close(classrel, RowExclusiveLock);
}

/*
 * Remove mappings belonging to obsolete restricted-slot incarnations.
 *
 * Build the set of valid restricted slots in the current database, identified
 * by name and incarnation, and delete mappings whose owner is no longer in
 * that set.  After deleting mappings, clear relhasrestrictedslots only for
 * relations that have no remaining mapping.
 *
 * Required mapping additions are always performed synchronously by the
 * transaction that expands a scope.  Cleanup must not attempt to reconstruct
 * missing mappings after logical WAL could already have been omitted.
 * Mappings made obsolete by publication contraction may remain indefinitely,
 * since they cause only conservative extra WAL.
 */
void
LogicalSlotScopeCleanup(void)
{
	List	   *slots = NIL;
	List	   *changed_relations = NIL;
	Relation	maprel;
	TableScanDesc scan;
	HeapTuple	tuple;

	lock_database_scope();
	LWLockAcquire(ReplicationSlotAllocationLock, LW_SHARED);
	for (int i = 0; i < max_replication_slots + max_repack_replication_slots; i++)
	{
		ReplicationSlot *slot = &ReplicationSlotCtl->replication_slots[i];
		ReplicationSlotPersistentData data;
		ReconcileSlot *item;

		if (!slot->in_use)
			continue;
		SpinLockAcquire(&slot->mutex);
		data = slot->data;
		SpinLockRelease(&slot->mutex);
		if (data.database != MyDatabaseId || data.unrestricted ||
			data.invalidated != RS_INVAL_NONE)
			continue;
		item = palloc(sizeof(*item));
		item->name = data.name;
		item->incarnation = data.restricted_scope_incarnation;
		slots = lappend(slots, item);
	}
	LWLockRelease(ReplicationSlotAllocationLock);

	/*
	 * Required additions are synchronous in their originating transaction;
	 * maintenance must never try to repair them after WAL could be lost. Keep
	 * this pass bounded to removing mappings for slots that no longer exist.
	 * Publication contraction can safely leave conservative entries.
	 */
	maprel = table_open(RestrictedSlotRelationRelationId, RowExclusiveLock);
	scan = table_beginscan_catalog(maprel, 0, NULL);
	while (HeapTupleIsValid(tuple = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_restricted_slot_relation form =
			(Form_pg_restricted_slot_relation) GETSTRUCT(tuple);

		if (!slot_identity_list_contains(slots, NameStr(form->rsrslotname),
										 (uint64) form->rsrincarnation))
		{
			if (OidIsValid(form->rsrrelid))
				changed_relations = list_append_unique_oid(changed_relations,
														   form->rsrrelid);
			CatalogTupleDelete(maprel, &tuple->t_self);
		}
	}
	table_endscan(scan);
	table_close(maprel, RowExclusiveLock);
	CommandCounterIncrement();
	foreach_oid(relid, changed_relations)
		clear_relation_restricted_flag(relid);
	list_free(changed_relations);
	list_free_deep(slots);
	RequestDisableLogicalDecoding();
}

/* Resolve publication names and prepare a restricted slot for mappings. */
void
LogicalSlotScopePrepareFromPublications(ReplicationSlot *slot, List *pubnames)
{
	List	   *publications = NIL;

	Assert(IsTransactionState());
	Assert(pubnames != NIL);
	foreach_ptr(char, pubname, pubnames)
	{
		Publication *pub = GetPublicationByName(pubname, false);

		if (pub->alltables)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("publication \"%s\" is defined FOR ALL TABLES", pubname),
					 errhint("Create an unrestricted logical replication slot instead.")));
		publications = lappend_oid(publications, pub->oid);
	}
	list_sort(publications, list_oid_cmp);
	list_deduplicate_oid(publications);
	LogicalSlotScopePrepareFromPublicationOids(slot, publications);
	list_free(publications);
}

/*
 * Prepare a slot for transactional restricted-scope initialization.
 *
 * Assign a new nonzero incarnation, mark the slot not ready, and persist its
 * publication identities.  Enable logical decoding and temporarily require
 * full logical WAL so that concurrent changes cannot be missed before the
 * initial relation mappings commit.  Also enable restricted logical WAL for
 * the steady state after the temporary full-WAL requirement is removed.
 */
void
LogicalSlotScopePrepareFromPublicationOids(ReplicationSlot *slot,
										   List *publications)
{
	uint64		incarnation;

	Assert(IsTransactionState());
	Assert(publications != NIL);
	lock_database_scope();
	check_no_database_scope_change();
	do
	{
		incarnation = pg_prng_uint64(&pg_global_prng_state);
	} while (incarnation == 0);

	/*
	 * Install the publication file before making the restricted slot visible
	 * to concurrent scope-expansion hooks.  Such hooks may then safely
	 * include a not-yet-ready slot.
	 */
	write_publications_file(slot, publications);

	/* Enable logical decoding and WAL */
	EnsureLogicalDecodingEnabled();

	/* Temporarily raise effective_wal_level to logical */
	EnsureFullLogicalWAL();

	/*
	 * Raise restricted_wal_level to logical, but before
	 * LogicalSlotScopeFinishCreate, the slot is not yet ready.
	 */
	EnsureRestrictedLogicalWAL();

	SpinLockAcquire(&slot->mutex);
	slot->data.unrestricted = false;
	slot->data.restricted_scope_ready = false;
	slot->data.restricted_scope_incarnation = incarnation;
	slot->data.restricted_scope_ready_lsn = InvalidXLogRecPtr;
	SpinLockRelease(&slot->mutex);
	ReplicationSlotMarkDirty();
}

/*
 * Install the initial mappings for a prepared restricted slot.
 *
 * Re-read and expand the stored publications, add mappings for their current
 * physical relation closure, and insert the readiness marker in the same
 * transaction.  Queue the slot for transaction-end finalization; it must not
 * be marked ready until that transaction commits.
 *
 * If finalize_at_xact_end is true, the transaction callback also releases
 * the slot after commit or abort.
 */
void
LogicalSlotScopeFinishCreate(ReplicationSlot *slot, bool finalize_at_xact_end)
{
	List	   *publications;
	List	   *relations;
	MemoryContext oldcontext;
	PendingReadySlot *pending;

	Assert(IsTransactionState());
	Assert(!slot->data.unrestricted);

	lock_database_scope();
	check_no_database_scope_change();
	publications = read_publications_file(slot);
	relations = publication_relation_closure(publications, true);
	logical_slot_scope_add_relations(NameStr(slot->data.name), relations, true);
	restricted_ready_marker_add(NameStr(slot->data.name),
								slot->data.restricted_scope_incarnation);
	ReplicationSlotMarkDirty();
	oldcontext = MemoryContextSwitchTo(TopTransactionContext);
	pending = palloc(sizeof(*pending));
	pending->slot = slot;
	pending->name = slot->data.name;
	pending->finalize_at_xact_end = finalize_at_xact_end;
	/* pending_ready_slots will be processed by the transaction callback */
	pending_ready_slots = lappend(pending_ready_slots, pending);
	MemoryContextSwitchTo(oldcontext);
	list_free(relations);
	list_free(publications);
}

/*
 * Restore the scope metadata of a synchronized restricted slot.
 *
 * Install the publication identities and upstream slot incarnation, mark the
 * slot ready at the supplied mapping-durability boundary, and enable
 * restricted logical WAL.  The caller must ensure that WAL through ready_lsn
 * has been flushed locally, so the corresponding transactional catalog
 * mappings are available before the synchronized slot becomes usable.
 */
void
LogicalSlotScopeRestorePublications(ReplicationSlot *slot, List *publications,
									uint64 incarnation, XLogRecPtr ready_lsn)
{
	write_publications_file(slot, publications);
	SpinLockAcquire(&slot->mutex);
	slot->data.unrestricted = false;
	slot->data.restricted_scope_ready = true;
	slot->data.restricted_scope_incarnation = incarnation;
	slot->data.restricted_scope_ready_lsn = ready_lsn;
	SpinLockRelease(&slot->mutex);
	EnableLogicalDecoding();
	EnsureRestrictedLogicalWAL();
	ReplicationSlotMarkDirty();
}

/* Configure the writer-WAL requirement for an unrestricted slot. */
void
LogicalSlotScopeConfigureUnrestricted(ReplicationSlot *slot)
{
	SpinLockAcquire(&slot->mutex);
	slot->data.unrestricted = true;
	slot->data.restricted_scope_ready = true;
	slot->data.restricted_scope_ready_lsn = InvalidXLogRecPtr;
	SpinLockRelease(&slot->mutex);
	EnsureLogicalDecodingEnabled();
}

/*
 * Restore writer-side WAL requirements for a logical slot loaded from disk.
 *
 * A ready restricted slot requires restricted logical WAL.  An incomplete
 * restricted slot temporarily requires full logical WAL until database-level
 * reconciliation either completes or drops it.  Unrestricted slots require
 * no scope-specific restoration here.
 */
void
LogicalSlotScopeRestore(ReplicationSlot *slot)
{
	if (!SlotIsLogical(slot))
		return;
	if (!slot->data.unrestricted && slot->data.restricted_scope_ready)
	{
		EnableLogicalDecoding();
		EnsureRestrictedLogicalWAL();
	}
	else if (!slot->data.unrestricted)
	{
		EnableLogicalDecoding();
		EnsureFullLogicalWAL();
	}
}

/*
 * Propagate restricted-slot mappings across a new partition hierarchy link.
 *
 * For every restricted slot mapped to the parent, add mappings for the child,
 * its partition descendants, and their TOAST relations.  These changes occur
 * in the transaction that creates the hierarchy link, so the new members
 * require logical WAL as soon as the link becomes visible.
 *
 * Mapping removal after detach is deliberately deferred, since a stale
 * mapping causes only conservative extra WAL.
 */
void
CheckLogicalSlotScopeHierarchyChange(Oid childrelid, Oid parentrelid)
{
	List	   *slotnames;
	List	   *roots;
	List	   *closure;

	mark_database_scope_change();
	lock_database_scope();
	slotnames = restricted_slots_for_relation(parentrelid);
	unlock_database_scope();
	if (slotnames == NIL)
	{
		return;
	}
	list_free_deep(slotnames);

	roots = list_make1_oid(childrelid);
	closure = relation_physical_closure(roots);
	lock_database_scope();
	/* Recheck after taking relation locks and reacquiring serialization. */
	slotnames = restricted_slots_for_relation(parentrelid);
	if (slotnames == NIL)
	{
		list_free(closure);
		list_free(roots);
		unlock_database_scope();
		return;
	}
	EnsureRestrictedLogicalWAL();
	foreach_ptr(char, slotname, slotnames)
		logical_slot_scope_add_relations(slotname, closure, false);
	list_free(closure);
	list_free(roots);
	list_free_deep(slotnames);
	unlock_database_scope();
}

/*
 * Return whether changes to a relation require logical tuple information.
 *
 * Relations that cannot participate in logical decoding never require it.
 * Full logical WAL covers every eligible relation.  In restricted mode, use
 * the relcache copy of pg_class.relhasrestrictedslots, avoiding catalog
 * access in the WAL insertion path.
 */
bool
RelationNeedsLogicalTupleWAL(Relation relation)
{
	if (!RelationCanBeLogicallyLogged(relation))
		return false;
	if (XLogFullLogicalInfoActive())
		return true;
	if (!XLogRestrictedInfoActive())
		return false;
	return relation->rd_rel->relhasrestrictedslots;
}

/*
 * Return whether a slot permits all requested publications.
 *
 * Unrestricted slots permit any publication.  For a restricted slot, resolve
 * each requested publication name to its current OID and require that OID to
 * appear in the immutable publication set stored by the slot.  This validates
 * publication identity only; normal pgoutput processing determines current
 * relation membership.
 */
bool
LogicalSlotScopePublicationsContain(ReplicationSlot *slot, List *pubnames)
{
	List	   *stored;
	bool		result = true;

	if (slot->data.unrestricted)
		return true;
	stored = read_publications_file(slot);
	foreach_ptr(char, pubname, pubnames)
	{
		Publication *pub = GetPublicationByName(pubname, false);

		if (!list_member_oid(stored, pub->oid))
		{
			result = false;
			break;
		}
	}
	list_free(stored);
	return result;
}
