/*-------------------------------------------------------------------------
 *
 * rewrite.c
 *		Rewriting a catalog at an in-place major version upgrade
 *
 * A catalog is rewritten into a new file: its pages are read through the
 * buffer manager (see oldcatalog.c), each row is converted to this version's
 * layout (see convert_row()), and the rows are written out frozen through
 * bulk_write, which WAL-logs every page in full, so that a standby gets the
 * whole result through WAL.  The catalog's indexes are built from the rows as
 * they are written: an entry for each index is spooled while the rows
 * stream through, and nbtsort.c sorts the spools and writes the trees
 * afterwards.  A catalog's toast table is rewritten the same way; the toast
 * pointers in the catalog's rows stay valid, since they name the toast
 * table by OID and the chunks by value ID.
 *
 * Nothing here reads a catalog through the relation cache: the relation
 * descriptors that tuplesort and nbtsort.c need are built from
 * catalog_relations.h.  A row that is not frozen is an error, because the
 * rows are read without a snapshot (the procedure driving the upgrade
 * freezes the catalogs before the old major version shuts down).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/upgrade/rewrite.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "catalog/catalog_relations.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_authid_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/storage_xlog.h"
#include "common/int.h"
#include "miscadmin.h"
#include "storage/bulk_write.h"
#include "storage/smgr.h"
#include "upgrade/inplace_upgrade.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/rel.h"

/* Writes rows into a new heap file, page by page, through bulk_write */
typedef struct HeapWriter
{
	BulkWriteState *bulkstate;
	BulkWriteBuffer buf;		/* the page being filled, or NULL */
	BlockNumber blkno;			/* its block number */
	uint64		ntuples;
} HeapWriter;

/* An index of the relation being rewritten, under construction */
typedef struct IndexBuild
{
	const CatalogIndex *info;
	Relation	indexrel;
	BTSpool    *spool;
} IndexBuild;

/* The state of rewrite_relation(), for rewrite_tuple() */
typedef struct CatalogRewrite
{
	const MigrateRelation *rel;
	const CatalogRelation *cat; /* NULL for a toast table */
	TupleDesc	tupdesc;
	const FilenodeFixup *fixups;
	int			nfixups;
	HeapWriter	writer;
	IndexBuild *builds;
	int			nindexes;
	Datum	   *values;
	bool	   *isnull;
} CatalogRewrite;

static void rewrite_relation(const char *relname, TupleDesc tupdesc,
							 const MigrateRelation *rel,
							 const CatalogIndex *indexinfos,
							 const MigrateRelation *indexrels, int nindexes,
							 const CatalogRelation *cat,
							 const FilenodeFixup *fixups, int nfixups);
static void rewrite_tuple(HeapTuple oldtuple, void *arg);
static void convert_row(const CatalogRelation *cat, HeapTuple tuple);
static void fix_relfilenode(HeapTuple tuple, const FilenodeFixup *fixups,
							int nfixups);
static int	fixup_cmp(const void *key, const void *entry);
static void freeze_tuple(HeapTupleHeader htup);
static TupleDesc build_tupledesc(int natts, const FormData_pg_attribute *attrs);
static Relation build_heap_relation(const char *relname,
									const MigrateRelation *rel,
									TupleDesc tupdesc);
static Relation build_index_relation(Relation heaprel,
									 const CatalogIndex *info,
									 const MigrateRelation *rel);
static SMgrRelation create_storage(const MigrateRelation *rel);
static void heapwriter_init(HeapWriter *w, SMgrRelation srel);
static ItemPointerData heapwriter_add(HeapWriter *w, HeapTuple tuple);
static void heapwriter_flush(HeapWriter *w);
static void heapwriter_finish(HeapWriter *w);


/*
 * Rewrite a catalog, its indexes and its toast table with its index, each
 * into the new file that the caller allocated.  The relations are given in
 * the order of catalog_relations.h: indexes[i] is cat->indexes[i].  The
 * fixups are the new file numbers to store in the rows of pg_class, and
 * are used only when cat is pg_class.
 */
void
InplaceUpgradeRewriteCatalog(const CatalogRelation *cat,
							 const MigrateRelation *heap,
							 const MigrateRelation *indexes,
							 const MigrateRelation *toast,
							 const MigrateRelation *toastindex,
							 const FilenodeFixup *fixups, int nfixups)
{
	MemoryContext rewrite_cxt;
	MemoryContext oldcxt;
	TupleDesc	tupdesc;

	StaticAssertStmt(CATALOG_INDEX_NSUPPORT == BTNProcs,
					 "catalog_relations.h lists the wrong number of btree support procedures");

	rewrite_cxt = AllocSetContextCreate(CurrentMemoryContext,
										"catalog rewrite",
										ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(rewrite_cxt);

	set_ps_display(psprintf("rewriting %s", cat->name));

	tupdesc = build_tupledesc(cat->natts, cat->attrs);
	rewrite_relation(cat->name, tupdesc, heap, cat->indexes, indexes,
					 cat->nindexes, cat, fixups, nfixups);

	if (toast != NULL)
	{
		tupdesc = build_tupledesc(lengthof(CatalogToastAttrs),
								  CatalogToastAttrs);
		rewrite_relation(toast->name, tupdesc, toast, &CatalogToastIndex,
						 toastindex, 1, NULL, NULL, 0);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(rewrite_cxt);
}

/*
 * Rewrite one heap relation and build its indexes.  cat is NULL for a toast
 * table, whose rows are copied as they are.
 */
static void
rewrite_relation(const char *relname, TupleDesc tupdesc,
				 const MigrateRelation *rel,
				 const CatalogIndex *indexinfos,
				 const MigrateRelation *indexrels, int nindexes,
				 const CatalogRelation *cat,
				 const FilenodeFixup *fixups, int nfixups)
{
	CatalogRewrite state;
	Relation	heaprel;
	SMgrRelation newsrel;

	heaprel = build_heap_relation(relname, rel, tupdesc);

	state.rel = rel;
	state.cat = cat;
	state.tupdesc = tupdesc;
	state.fixups = fixups;
	state.nfixups = nfixups;
	state.nindexes = nindexes;
	state.values = palloc_array(Datum, tupdesc->natts);
	state.isnull = palloc_array(bool, tupdesc->natts);

	/*
	 * Each index gets its spool now, and all of them are filled in the one
	 * pass over the old heap below: an index entry needs the row's TID in the
	 * new heap, which is known only once heapwriter_add() has placed the row.
	 * The indexes' files are created up front, like the heap's, so that every
	 * file this rewrite creates is registered before a row is read.
	 */
	state.builds = palloc_array(IndexBuild, nindexes);
	for (int i = 0; i < nindexes; i++)
	{
		IndexBuild *build = &state.builds[i];

		build->info = &indexinfos[i];
		build->indexrel = build_index_relation(heaprel, build->info,
											   &indexrels[i]);
		smgrclose(create_storage(&indexrels[i]));

		/*
		 * The rows come from a consistent cluster, so a unique index has no
		 * duplicates to find.  Not enforcing uniqueness here keeps the
		 * duplicate-key report, which looks up the catalogs, out of reach.
		 */
		build->spool = _bt_spoolinit(heaprel, build->indexrel, false, false);
	}

	newsrel = create_storage(rel);
	heapwriter_init(&state.writer, newsrel);

	InplaceUpgradeScanOldCatalog(rel->oldlocator, relname, rewrite_tuple,
								 &state);

	heapwriter_finish(&state.writer);
	smgrclose(newsrel);

	for (int i = 0; i < nindexes; i++)
	{
		_bt_leafbuild(state.builds[i].spool, NULL);
		_bt_spooldestroy(state.builds[i].spool);
		RelationCloseSmgr(state.builds[i].indexrel);
	}

	ereport(DEBUG1,
			(errmsg_internal("rewrote \"%s\": %" PRIu64 " tuples in %u pages, %d indexes",
							 relname, state.writer.ntuples, state.writer.blkno,
							 nindexes)));
}

/*
 * Write one row of the old heap into the new one, and spool its entries
 * for the indexes.
 */
static void
rewrite_tuple(HeapTuple oldtuple, void *arg)
{
	CatalogRewrite *state = arg;
	HeapTupleData tuple;
	Datum		keyvalues[CATALOG_INDEX_MAX_KEYS];
	bool		keynulls[CATALOG_INDEX_MAX_KEYS];

	/* Copy the row out of the buffer, and convert the copy. */
	tuple.t_len = oldtuple->t_len;
	tuple.t_data = palloc(tuple.t_len);
	memcpy(tuple.t_data, oldtuple->t_data, tuple.t_len);
	tuple.t_self = oldtuple->t_self;
	tuple.t_tableOid = state->rel->reloid;

	if (state->cat != NULL)
	{
		convert_row(state->cat, &tuple);
		if (state->cat->reloid == RelationRelationId)
			fix_relfilenode(&tuple, state->fixups, state->nfixups);
	}
	freeze_tuple(tuple.t_data);

	tuple.t_self = heapwriter_add(&state->writer, &tuple);

	if (state->nindexes > 0)
	{
		heap_deform_tuple(&tuple, state->tupdesc, state->values,
						  state->isnull);
		for (int i = 0; i < state->nindexes; i++)
		{
			const CatalogIndex *info = state->builds[i].info;

			for (int k = 0; k < info->nkeys; k++)
			{
				int			attnum = info->keys[k].attnum;

				keyvalues[k] = state->values[attnum - 1];
				keynulls[k] = state->isnull[attnum - 1];
			}
			_bt_spool(state->builds[i].spool, &tuple.t_self,
					  keyvalues, keynulls);
		}
	}

	pfree(tuple.t_data);
}

/*
 * Convert a row of the old major version's catalog into a row of this
 * version's.  The row is a private copy, so it can be changed in place or
 * replaced.
 *
 * This version's catalogs have the same layout as the old version's, so a
 * row is taken as it is.  A version that changes a catalog's layout
 * converts the row here.
 */
static void
convert_row(const CatalogRelation *cat, HeapTuple tuple)
{
}

/*
 * A rewritten relation that is not in the relation map gets its new file
 * number in its pg_class row.
 */
static void
fix_relfilenode(HeapTuple tuple, const FilenodeFixup *fixups, int nfixups)
{
	Form_pg_class classform = (Form_pg_class) GETSTRUCT(tuple);
	const FilenodeFixup *fixup;

	if (nfixups == 0)
		return;
	fixup = bsearch(&classform->oid, fixups, nfixups, sizeof(FilenodeFixup),
					fixup_cmp);
	if (fixup != NULL)
		classform->relfilenode = fixup->relfilenode;
}

static int
fixup_cmp(const void *key, const void *entry)
{
	return pg_cmp_u32(*(const Oid *) key, ((const FilenodeFixup *) entry)->reloid);
}

/*
 * Mark a row as inserted by a transaction older than any that will ever
 * run, and as never updated.  Any of the old version's transactions that
 * touched it has long committed: the row was checked to be frozen.
 */
static void
freeze_tuple(HeapTupleHeader htup)
{
	htup->t_infomask &= ~HEAP_XACT_MASK;
	htup->t_infomask2 &= ~HEAP2_XACT_MASK;
	htup->t_infomask |= HEAP_XMIN_FROZEN | HEAP_XMAX_INVALID;
	HeapTupleHeaderSetXmin(htup, FrozenTransactionId);
	HeapTupleHeaderSetXmax(htup, InvalidTransactionId);
	HeapTupleHeaderSetCmin(htup, FirstCommandId);
}

/*
 * A tuple descriptor from the fixed-length parts of pg_attribute rows, as
 * relcache.c builds the bootstrap catalogs' from schemapg.h.
 */
static TupleDesc
build_tupledesc(int natts, const FormData_pg_attribute *attrs)
{
	TupleDesc	tupdesc = CreateTemplateTupleDesc(natts);

	for (int i = 0; i < natts; i++)
	{
		memcpy(TupleDescAttr(tupdesc, i), &attrs[i],
			   ATTRIBUTE_FIXED_PART_SIZE);
		populate_compact_attribute(tupdesc, i);
	}
	TupleDescFinalize(tupdesc);

	return tupdesc;
}

/*
 * A relation descriptor of the new heap file, with what tuplesort and
 * nbtsort.c read of it: the name, the descriptor, and the file.
 */
static Relation
build_heap_relation(const char *relname, const MigrateRelation *rel,
					TupleDesc tupdesc)
{
	Relation	relation = palloc0_object(RelationData);

	relation->rd_refcnt = 1;
	relation->rd_isvalid = true;
	relation->rd_backend = INVALID_PROC_NUMBER;
	relation->rd_id = rel->reloid;
	relation->rd_locator = rel->newlocator;
	relation->rd_att = tupdesc;

	relation->rd_rel = palloc0(CLASS_TUPLE_SIZE);
	relation->rd_rel->oid = rel->reloid;
	namestrcpy(&relation->rd_rel->relname, relname);
	relation->rd_rel->relnamespace = PG_CATALOG_NAMESPACE;
	relation->rd_rel->relowner = BOOTSTRAP_SUPERUSERID;
	relation->rd_rel->relfilenode = rel->mapped ? InvalidRelFileNumber :
		rel->newlocator.relNumber;
	relation->rd_rel->relisshared = !OidIsValid(rel->newlocator.dbOid);
	relation->rd_rel->relpersistence = RELPERSISTENCE_PERMANENT;
	relation->rd_rel->relkind = RELKIND_RELATION;
	relation->rd_rel->relnatts = tupdesc->natts;
	relation->rd_rel->reltuples = -1;

	return relation;
}

/*
 * A relation descriptor of a new btree index, with what tuplesort and
 * nbtsort.c read of it: besides the heap's part, the pg_index row, the
 * index tuple descriptor, and the operator class information that the
 * relation cache would load from pg_opclass and pg_amproc.
 */
static Relation
build_index_relation(Relation heaprel, const CatalogIndex *info,
					 const MigrateRelation *rel)
{
	Relation	relation = palloc0_object(RelationData);
	TupleDesc	heapdesc = RelationGetDescr(heaprel);
	Form_pg_index index;
	int			nkeys = info->nkeys;
	int			nsupport;
	MemoryContext oldcxt;

	relation->rd_refcnt = 1;
	relation->rd_isvalid = true;
	relation->rd_backend = INVALID_PROC_NUMBER;
	relation->rd_id = rel->reloid;
	relation->rd_locator = rel->newlocator;

	relation->rd_rel = palloc0(CLASS_TUPLE_SIZE);
	relation->rd_rel->oid = rel->reloid;
	namestrcpy(&relation->rd_rel->relname, rel->name);
	relation->rd_rel->relnamespace = PG_CATALOG_NAMESPACE;
	relation->rd_rel->relowner = BOOTSTRAP_SUPERUSERID;
	relation->rd_rel->relam = BTREE_AM_OID;
	relation->rd_rel->relfilenode = rel->mapped ? InvalidRelFileNumber :
		rel->newlocator.relNumber;
	relation->rd_rel->relisshared = heaprel->rd_rel->relisshared;
	relation->rd_rel->relpersistence = RELPERSISTENCE_PERMANENT;
	relation->rd_rel->relkind = RELKIND_INDEX;
	relation->rd_rel->relnatts = nkeys;
	relation->rd_rel->reltuples = -1;

	index = palloc0(sizeof(FormData_pg_index));
	index->indexrelid = rel->reloid;
	index->indrelid = RelationGetRelid(heaprel);
	index->indnatts = nkeys;
	index->indnkeyatts = nkeys;
	index->indisunique = info->unique;
	index->indisprimary = false;
	index->indimmediate = true;
	index->indisvalid = true;
	index->indisready = true;
	index->indislive = true;
	relation->rd_index = index;

	/*
	 * The index tuple descriptor: each key column as in the heap, except that
	 * an operator class may store a key as another type.
	 */
	relation->rd_att = CreateTemplateTupleDesc(nkeys);
	for (int i = 0; i < nkeys; i++)
	{
		const CatalogIndexKey *key = &info->keys[i];
		Form_pg_attribute att = TupleDescAttr(relation->rd_att, i);

		memcpy(att, TupleDescAttr(heapdesc, key->attnum - 1),
			   ATTRIBUTE_FIXED_PART_SIZE);
		att->attrelid = rel->reloid;
		att->attnum = i + 1;
		att->attcollation = key->collation;
		att->attnotnull = false;
		att->atthasdef = false;
		att->atthasmissing = false;
		att->attidentity = '\0';
		att->attgenerated = '\0';
		if (OidIsValid(key->keytype))
		{
			att->atttypid = key->keytype;
			att->attlen = key->keytyplen;
			att->attbyval = key->keytypbyval;
			att->attalign = key->keytypalign;
			att->attstorage = key->keytypstorage;
		}
		populate_compact_attribute(relation->rd_att, i);
	}
	TupleDescFinalize(relation->rd_att);

	relation->rd_indexcxt = AllocSetContextCreate(CurrentMemoryContext,
												  "index info",
												  ALLOCSET_SMALL_SIZES);
	MemoryContextCopyAndSetIdentifier(relation->rd_indexcxt,
									  RelationGetRelationName(relation));
	oldcxt = MemoryContextSwitchTo(relation->rd_indexcxt);
	relation->rd_indam = GetIndexAmRoutine(F_BTHANDLER);
	nsupport = relation->rd_indam->amsupport;
	Assert(nsupport == CATALOG_INDEX_NSUPPORT);
	relation->rd_opfamily = palloc_array(Oid, nkeys);
	relation->rd_opcintype = palloc_array(Oid, nkeys);
	relation->rd_indcollation = palloc_array(Oid, nkeys);
	relation->rd_indoption = palloc0_array(int16, nkeys);
	relation->rd_support = palloc0_array(RegProcedure, nkeys * nsupport);
	relation->rd_supportinfo = palloc0_array(FmgrInfo, nkeys * nsupport);
	for (int i = 0; i < nkeys; i++)
	{
		const CatalogIndexKey *key = &info->keys[i];

		relation->rd_opfamily[i] = key->opfamily;
		relation->rd_opcintype[i] = key->opcintype;
		relation->rd_indcollation[i] = key->collation;
		for (int j = 0; j < nsupport; j++)
			relation->rd_support[i * nsupport + j] = key->support[j];
	}
	MemoryContextSwitchTo(oldcxt);

	return relation;
}

/*
 * Create the main fork of a new relation file, WAL-logged, after
 * registering it for a rollback.
 */
static SMgrRelation
create_storage(const MigrateRelation *rel)
{
	RelPathStr	path = relpathperm(rel->newlocator, MAIN_FORKNUM);
	SMgrRelation srel;

	InplaceUpgradeRegisterCreatedFile(path.str);

	srel = smgropen(rel->newlocator, INVALID_PROC_NUMBER);
	smgrcreate(srel, MAIN_FORKNUM, false);
	log_smgrcreate(&rel->newlocator, MAIN_FORKNUM);

	return srel;
}

/*
 * Writing a heap file page by page
 */
static void
heapwriter_init(HeapWriter *w, SMgrRelation srel)
{
	w->bulkstate = smgr_bulk_start_smgr(srel, MAIN_FORKNUM, true);
	w->buf = NULL;
	w->blkno = 0;
	w->ntuples = 0;
}

/*
 * Add a frozen tuple to the page being filled, starting a new page if it
 * does not fit.  Returns where the tuple landed; its t_ctid points there.
 */
static ItemPointerData
heapwriter_add(HeapWriter *w, HeapTuple tuple)
{
	Page		page;
	OffsetNumber offnum;
	ItemPointerData tid;

	if (w->buf != NULL &&
		PageGetHeapFreeSpace((Page) w->buf) < MAXALIGN(tuple->t_len))
		heapwriter_flush(w);
	if (w->buf == NULL)
	{
		w->buf = smgr_bulk_get_buf(w->bulkstate);
		PageInit((Page) w->buf, BLCKSZ, 0);
	}
	page = (Page) w->buf;

	offnum = PageAddItemExtended(page, tuple->t_data, tuple->t_len,
								 InvalidOffsetNumber, PAI_IS_HEAP);
	if (offnum == InvalidOffsetNumber)
		elog(ERROR, "could not add tuple of %u bytes to a new page",
			 tuple->t_len);

	ItemPointerSet(&tid, w->blkno, offnum);
	((HeapTupleHeader) PageGetItem(page, PageGetItemId(page, offnum)))->t_ctid = tid;
	w->ntuples++;

	return tid;
}

/* Hand the page being filled to bulk_write. */
static void
heapwriter_flush(HeapWriter *w)
{
	smgr_bulk_write(w->bulkstate, w->blkno, w->buf, true);
	w->buf = NULL;
	w->blkno++;
}

static void
heapwriter_finish(HeapWriter *w)
{
	if (w->buf != NULL)
		heapwriter_flush(w);
	smgr_bulk_finish(w->bulkstate);
}
