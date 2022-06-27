/*-------------------------------------------------------------------------
 *
 * lrg.c
 *		  Constructs a logical replication group
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_lrg_info.h"
#include "catalog/pg_lrg_nodes.h"
#include "catalog/pg_lrg_sub.h"
#include "catalog/pg_subscription.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "replication/libpqlrg.h"
#include "replication/logicallauncher.h"
#include "replication/lrg.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

LrgWorkerCtxStruct *LrgWorkerCtx;

static Size lrg_worker_array_size(void);
static Oid lrg_add_info(const char *group_name, const char *publication_type);
static Oid find_subscription(const char *subname);

/*
 * Helpler function for LrgLauncherShmemInit.
 */
static Size
lrg_worker_array_size(void)
{
	Size size;

	size = sizeof(LrgWorkerCtxStruct);
	size = MAXALIGN(size);
	/* XXX: Which value is appropriate for the array size? */
	size = add_size(size, mul_size(max_worker_processes, sizeof(LrgWorkerCtxStruct)));

	return size;
}

/*
 * Allocate LrgWorkerCtxStruct to the shared memory.
 */
void
LrgLauncherShmemInit(void)
{
	bool		found;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	LrgWorkerCtx = (LrgWorkerCtxStruct *)
		ShmemInitStruct("Lrg Launcher Data",
						lrg_worker_array_size(),
						&found);
	if (!found)
	{
		MemSet(LrgWorkerCtx, 0, lrg_worker_array_size());
		LWLockInitialize(&(LrgWorkerCtx->lock), LWLockNewTrancheId());
	}
	LWLockRelease(AddinShmemInitLock);
	LWLockRegisterTranche(LrgWorkerCtx->lock.tranche, "lrg");
}

/*
 * Register the LRG launcher. This will be called during postmaster startup.
 */
void
LrgLauncherRegister(void)
{
	BackgroundWorker worker;

	/*
	 * LRG deeply depends on the logical replication mechanism, so
	 * skip registering the LRG launcher if logical replication
	 * cannot be used.
	 */
	if (max_logical_replication_workers == 0)
		return;

	/*
	 * Build struct BackgroundWorker for launcher.
	 */
	MemSet(&worker, 0, sizeof(BackgroundWorker));

	snprintf(worker.bgw_name, BGW_MAXLEN, "lrg launcher");
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(worker.bgw_library_name, BGW_MAXLEN, "postgres");
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "lrg_launcher_main");
	RegisterBackgroundWorker(&worker);
}

/*
 * construct node_id.
 *
 * TODO: construct proper node_id. Currently it is just concat of
 * sytem identifier and dbid.
 */
void
construct_node_id(char *out_node_id, int size)
{
	snprintf(out_node_id, size, UINT64_FORMAT "%u", GetSystemIdentifier(), MyDatabaseId);
}

/*
 * Actual work for adding a tuple to pg_lrg_nodes.
 */
void
lrg_add_nodes(const char *node_id, Oid group_id, LRG_NODE_STATE status,
			  const char *node_name, const char *local_connstring,
			  const char *upstream_connstring)
{
	Relation rel;
	bool		nulls[Natts_pg_lrg_nodes];
	Datum		values[Natts_pg_lrg_nodes];
	HeapTuple tup;

	Oid			lrgnodesoid;

	rel = table_open(LrgNodesRelationId, ExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	lrgnodesoid = GetNewOidWithIndex(rel, LrgNodesRelationIndexId, Anum_pg_lrg_nodes_oid);
	values[Anum_pg_lrg_nodes_oid - 1] = ObjectIdGetDatum(lrgnodesoid);
	values[Anum_pg_lrg_nodes_nodeid - 1] = CStringGetTextDatum(node_id);
	values[Anum_pg_lrg_nodes_groupid - 1] = ObjectIdGetDatum(group_id);
	values[Anum_pg_lrg_nodes_status - 1] = Int32GetDatum(status);
	values[Anum_pg_lrg_nodes_dbid - 1] = ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_lrg_nodes_nodename - 1] = CStringGetDatum(node_name);
	values[Anum_pg_lrg_nodes_localconn - 1] = CStringGetTextDatum(local_connstring);

	if (upstream_connstring != NULL)
		values[Anum_pg_lrg_nodes_upstreamconn - 1] = CStringGetTextDatum(upstream_connstring);
	else
		nulls[Anum_pg_lrg_nodes_upstreamconn - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	/* Insert tuple into catalog. */
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, ExclusiveLock);
}

/*
 * read pg_lrg_info and get oid.
 *
 * XXX: This function assumes that there is only one tuple
 * in the pg_lrg_info.
 */
Oid
get_group_info(char **group_name)
{
	Relation	rel;
	HeapTuple tup;
	TableScanDesc scan;
	Oid group_oid = InvalidOid;
	Form_pg_lrg_info infoform;
	bool is_opened = false;

	if (!IsTransactionState())
	{
		is_opened = true;
		StartTransactionCommand();
		(void) GetTransactionSnapshot();
	}

	rel = table_open(LrgInfoRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	tup = heap_getnext(scan, ForwardScanDirection);

	if (tup != NULL)
	{
		infoform = (Form_pg_lrg_info) GETSTRUCT(tup);
		group_oid = infoform->oid;
		if (group_name != NULL)
		{
			MemoryContext old;
			old = MemoryContextSwitchTo(TopMemoryContext);
			*group_name = pstrdup(NameStr(infoform->groupname));
			MemoryContextSwitchTo(old);
		}
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	if (is_opened)
		CommitTransactionCommand();

	return group_oid;
}

/*
 * Actual work for adding a tuple to pg_lrg_info.
 */
static Oid
lrg_add_info(const char *group_name, const char *publication_type)
{
	Relation	rel;
	bool		nulls[Natts_pg_lrg_info];
	Datum		values[Natts_pg_lrg_info];
	HeapTuple tup;
	Oid			lrgoid;

	rel = table_open(LrgInfoRelationId, ExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	lrgoid = GetNewOidWithIndex(rel, LrgInfoRelationIndexId, Anum_pg_lrg_info_oid);
	values[Anum_pg_lrg_info_oid - 1] = ObjectIdGetDatum(lrgoid);
	values[Anum_pg_lrg_info_groupname - 1] = CStringGetDatum(group_name);
	values[Anum_pg_lrg_info_pub_type - 1] = CStringGetTextDatum(publication_type);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	/* Insert tuple into catalog. */
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, ExclusiveLock);

	return lrgoid;
}

/*
 * helper function for lrg_insert_into_sub
 */
static Oid
find_subscription(const char *subname)
{
	/* for scannning */
	Relation rel;
	HeapTuple tup;
	Form_pg_subscription form;

	rel = table_open(SubscriptionRelationId, AccessExclusiveLock);
	tup = SearchSysCacheCopy2(SUBSCRIPTIONNAME, MyDatabaseId,
							  CStringGetDatum(subname));

	if (!HeapTupleIsValid(tup))
	{
		table_close(rel, NoLock);
		return InvalidOid;
	}

	form = (Form_pg_subscription) GETSTRUCT(tup);
	table_close(rel, NoLock);

	return form->oid;
}

/*
 * ================================
 * Public APIs
 * ================================
 */

/*
 * SQL function for creating a new logical replication group.
 *
 * This function adds a tuple to pg_lrg_info and pg_lrg_nodes,
 * and after that kick lrg launcher.
 */
Datum
lrg_create(PG_FUNCTION_ARGS)
{
	Oid			lrgoid;
	char		*group_name;
	char		*pub_type;
	char		*local_connstring;
	char		*node_name;
	char		*group_name_from_catalog = NULL;

	/* XXX: for simplify the fixed array is used */
	char		node_id[64];

	if (get_group_info(&group_name_from_catalog) != InvalidOid)
		ereport(ERROR,
				errmsg("could not create a node group"),
				errdetail("This node was already a member of %s.", group_name_from_catalog),
				errhint("You need to detach from or drop the group."));

	group_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	pub_type = text_to_cstring(PG_GETARG_TEXT_PP(1));

	if (pg_strcasecmp(pub_type, "FOR ALL TABLES") != 0)
		ereport(ERROR,
				errmsg("cannot create a node group"),
				errdetail("Only 'FOR ALL TABLES' is supported as publication type."));

	lrgoid = lrg_add_info(group_name, pub_type);

	construct_node_id(node_id, sizeof(node_id));
	local_connstring = text_to_cstring(PG_GETARG_TEXT_PP(2));
	node_name = text_to_cstring(PG_GETARG_TEXT_PP(3));
	lrg_add_nodes(node_id, lrgoid, LRG_STATE_INIT, node_name, local_connstring, NULL);

	lrg_launcher_wakeup();
	PG_RETURN_VOID();
}


/*
 * SQL function for attaching to a specified group
 *
 * This function adds a tuple to pg_lrg_info and pg_lrg_nodes,
 * and after that kicks lrg launcher.
 */
Datum
lrg_node_attach(PG_FUNCTION_ARGS)
{
	Oid			lrgoid;
	char		*group_name;
	char		*local_connstring;
	char		*upstream_connstring;
	char		*node_name;
	PGconn		*upstreamconn = NULL;
	char		*group_name_from_catalog = NULL;

	/* XXX: for simplify the fixed array is used */
	char		node_id[64];

	if (get_group_info(&group_name_from_catalog) != InvalidOid)
		ereport(ERROR,
				errmsg("could not attach to a node group"),
				errdetail("This node was already a member of %s.", group_name_from_catalog),
				errhint("You need to detach from or drop the group."));

	group_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	local_connstring = text_to_cstring(PG_GETARG_TEXT_PP(1));
	upstream_connstring = text_to_cstring(PG_GETARG_TEXT_PP(2));
	node_name = text_to_cstring(PG_GETARG_TEXT_PP(3));

	/*
	 * For sanity check the backend process must connect to the upstream node.
	 * libpqlrg shared library will be used for that.
	 */
	load_file("libpqlrg", false);
	lrg_connect(upstream_connstring, &upstreamconn, true);

	if (!lrg_check_group(upstreamconn, group_name))
		ereport(ERROR,
				errmsg("could not attach to the node group"),
				errdetail("Upstream node is not a member of specified group."));

	lrg_disconnect(upstreamconn);

	lrgoid = lrg_add_info(group_name, "FOR ALL TABLES");

	construct_node_id(node_id, sizeof(node_id));
	lrg_add_nodes(node_id, lrgoid, LRG_STATE_INIT, node_name, local_connstring, upstream_connstring);

	lrg_launcher_wakeup();
	PG_RETURN_VOID();
}

/*
 * SQL function for detaching from a group
 */
Datum
lrg_node_detach(PG_FUNCTION_ARGS)
{
	char		*node_name;
	char		*given_group_name;
	char		*group_name_from_catalog = NULL;
	bool		force_detach;

	given_group_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	node_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
	force_detach = PG_GETARG_BOOL(2);

	(void) get_group_info(&group_name_from_catalog);
	if (group_name_from_catalog == NULL)
		ereport(ERROR,
				errmsg("could not detach from the node group"),
				errdetail("This node was in any node groups."));
	else if (strcmp(given_group_name, group_name_from_catalog) != 0)
		ereport(ERROR,
				errmsg("could not detach from the node group"),
				errdetail("This node was in %s, but %s is specified.",
						  group_name_from_catalog, given_group_name));

	update_node_status_by_nodename(node_name, force_detach ? LRG_STATE_FORCE_DETACH : LRG_STATE_TO_BE_DETACHED, true);
	lrg_launcher_wakeup();
	PG_RETURN_VOID();
}

/*
 * SQL function for dropping a group.
 */
Datum
lrg_drop(PG_FUNCTION_ARGS)
{
	char node_id[64];
	char		*given_group_name;
	char		*group_name_from_catalog = NULL;

	construct_node_id(node_id, sizeof(node_id));

	given_group_name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	(void) get_group_info(&group_name_from_catalog);
	if (group_name_from_catalog == NULL)
		ereport(ERROR,
				errmsg("could not drop the node group"),
				errdetail("This node was in any node groups."));
	else if (strcmp(given_group_name, group_name_from_catalog) != 0)
		ereport(ERROR,
				errmsg("could not drop the node group"),
				errdetail("This node was in %s, but %s is specified.",
						  group_name_from_catalog, given_group_name));

	/* TODO: add a check whether there are not other members in the group or not  */
	update_node_status_by_nodeid(node_id, LRG_STATE_TO_BE_DETACHED, true);
	lrg_launcher_wakeup();
	PG_RETURN_VOID();
}

/*
 * Wait until lrg related functions are done
 *
 * Note that this function registers/unregisters a latest snapshot within a
 * loop This may be not consistent with the isolation level set by user.
 *
 * XXX: Should we add a timeout parameter?
 */
Datum
lrg_wait(PG_FUNCTION_ARGS)
{
	if (get_group_info(NULL) == InvalidOid)
		PG_RETURN_NULL();

	for (;;)
	{
		Relation	rel;
		HeapTuple	tup;
		SysScanDesc scan;
		/* Get latest snapshot in the every loop */
		Snapshot	latest = RegisterSnapshot(GetLatestSnapshot());
		bool need_more_loop = false;

		CHECK_FOR_INTERRUPTS();

		rel = table_open(LrgNodesRelationId, AccessShareLock);
		scan = systable_beginscan(rel, InvalidOid, false, latest, 0, NULL);

		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_lrg_nodes nodesform = (Form_pg_lrg_nodes) GETSTRUCT(tup);

			/*
			 * Set a flag if we must wait more.
			 */
			if (nodesform->status != LRG_STATE_READY)
				need_more_loop = true;
		}

		systable_endscan(scan);
		table_close(rel, NoLock);

		UnregisterSnapshot(latest);

		if (!need_more_loop)
			break;

		/*
		 * wait very short time...
		 */
#define TEMPORARY_NAP_TIME 500L
		WaitLatch(&MyProc->procLatch,
				  WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
				  TEMPORARY_NAP_TIME, 0);
	}

	PG_RETURN_VOID();
}

/*
 * ================================
 * Internal SQL functions
 * ================================
 */

/*
 * Wrapper for adding a tuple into pg_lrg_sub
 *
 * This function will be called by LRG worker.
 */
Datum
lrg_insert_into_sub(PG_FUNCTION_ARGS)
{
	char *sub_name;
	Oid group_oid, sub_oid, lrgsub_oid;
	Relation rel;
	bool		nulls[Natts_pg_lrg_sub];
	Datum		values[Natts_pg_lrg_sub];
	HeapTuple tup;

	sub_name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	group_oid = get_group_info(NULL);
	sub_oid = find_subscription(sub_name);

	rel = table_open(LrgSubscriptionId, ExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	lrgsub_oid = GetNewOidWithIndex(rel, LrgSubscriptionOidIndexId, Anum_pg_lrg_sub_oid);

	values[Anum_pg_lrg_sub_oid - 1] = ObjectIdGetDatum(lrgsub_oid);
	values[Anum_pg_lrg_sub_groupid - 1] = ObjectIdGetDatum(group_oid);
	values[Anum_pg_lrg_sub_subid - 1] = ObjectIdGetDatum(sub_oid);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	/* Insert tuple into catalog. */
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, ExclusiveLock);

	PG_RETURN_VOID();
}

/*
 * Wrapper for adding a tuple into pg_lrg_nodes
 *
 * This function will be called by LRG worker.
 */
Datum
lrg_insert_into_nodes(PG_FUNCTION_ARGS)
{
	char *node_id;
	LRG_NODE_STATE status;
	char *node_name;
	char *local_connstring;
	char *upstream_connstring;
	Oid group_oid;

	node_id = text_to_cstring(PG_GETARG_TEXT_PP(0));
	status = DatumGetInt32(PG_GETARG_DATUM(1));
	node_name = text_to_cstring(PG_GETARG_TEXT_PP(2));
	local_connstring = text_to_cstring(PG_GETARG_TEXT_PP(3));
	upstream_connstring = text_to_cstring(PG_GETARG_TEXT_PP(4));

	group_oid = get_group_info(NULL);

	lrg_add_nodes(node_id, group_oid, status, node_name, local_connstring, upstream_connstring);

	PG_RETURN_VOID();
}
