/*-------------------------------------------------------------------------
 *
 * lrg_worker.c
 *		  functions for lrg worker
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_lrg_info.h"
#include "catalog/pg_lrg_nodes.h"
#include "catalog/pg_lrg_pub.h"
#include "catalog/pg_publication.h"
#include "executor/spi.h"
#include "libpq-fe.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "replication/libpqlrg.h"
#include "replication/lrg.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * Data structure for one node.
 */
typedef struct LrgNode {
	Oid	  group_oid;
	char *node_id;
	char *node_name;
	char *local_connstring;
	char *upstream_connstring;
} LrgNode;

lrg_function_types *LrgFunctionTypes = NULL;

static LrgWorker* my_lrg_worker = NULL;

static void lrg_worker_onexit(int code, Datum arg);
static void do_node_management(void);
static void get_node_information(LrgNode **output_node, LRG_NODE_STATE *status);
static void advance_state_machine(LrgNode *node, LRG_NODE_STATE initial_status);
static void update_node_status_internal(const char *node_id, const char *node_name, LRG_NODE_STATE state, bool is_in_txn);
static void detach_node(LrgNode *node, bool force_detach);
static void create_publication(const char* group_name, const char* node_id, Oid group_oid);
static Oid find_publication(const char *pubname);
static List* get_lrg_nodes_list(const char *local_nodeid);
static void synchronise_system_tables(PGconn *localconn, PGconn *upstreamconn);

void
lrg_worker_cleanup(LrgWorker *worker)
{
	Assert(LWLockHeldByMeInMode(&LrgWorkerCtx->lock, LW_EXCLUSIVE));

	worker->dbid = InvalidOid;
	worker->worker_pid = InvalidPid;
	worker->worker_latch = NULL;
}

/*
 * Callback for process exit. cleanup the array.
 */
static void
lrg_worker_onexit(int code, Datum arg)
{
	LWLockAcquire(&LrgWorkerCtx->lock, LW_EXCLUSIVE);
	lrg_worker_cleanup(my_lrg_worker);
	LWLockRelease(&LrgWorkerCtx->lock);
}

/*
 * Synchronise system tables from upstream node.
 *
 * Currently it will read and insert pg_lrg_nodes only.
 */
static void
synchronise_system_tables(PGconn *localconn, PGconn *upstreamconn)
{
	lrg_copy_lrg_nodes(upstreamconn, localconn);
}

/*
 * Load the list of lrg_nodes, except the given node
 */
static List*
get_lrg_nodes_list(const char *excepted_node)
{
	List *res = NIL;
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tup;
	Snapshot 	current;
	/* We will allocate the output data in the current memory context */
	MemoryContext resultcxt = CurrentMemoryContext;

	StartTransactionCommand();
	current = GetTransactionSnapshot();

	rel = table_open(LrgNodesRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, current, 0, NULL);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		LrgNode			*node;
		MemoryContext	oldcxt;
		bool 			isnull;
		Datum			tmp_upstream_connstring;

		if (excepted_node != NULL &&
			strcmp(TextDatumGetCString(heap_getattr(tup, Anum_pg_lrg_nodes_nodeid, RelationGetDescr(rel), &isnull)), excepted_node) == 0)
			continue;
		/*
		 * Allocate our results in the caller's context, not the transaction's.
		 */
		oldcxt = MemoryContextSwitchTo(resultcxt);

		node = (LrgNode *)palloc0(sizeof(LrgNode));

		node->group_oid = heap_getattr(tup,
									   Anum_pg_lrg_nodes_groupid,
									   RelationGetDescr(rel),
									   &isnull);

		node->node_id = pstrdup(TextDatumGetCString(heap_getattr(tup,
															 Anum_pg_lrg_nodes_nodeid,
															 RelationGetDescr(rel),
															 &isnull)));
		node->node_name = pstrdup(DatumGetCString(heap_getattr(tup,
															   Anum_pg_lrg_nodes_nodename,
															   RelationGetDescr(rel),
															   &isnull)));
		node->local_connstring = pstrdup(TextDatumGetCString(heap_getattr(tup,
																		  Anum_pg_lrg_nodes_localconn,
																		  RelationGetDescr(rel),
																		  &isnull)));

		/*
		 * Unlike above attributes, upstreamconn might be NULL.
		 * So it must be substituted to the temporary variable
		 * and check whether it is null.
		 */
		tmp_upstream_connstring = heap_getattr(tup,
											   Anum_pg_lrg_nodes_upstreamconn,
											   RelationGetDescr(rel),
											   &isnull);

		if (!isnull)
			node->upstream_connstring = pstrdup(TextDatumGetCString(tmp_upstream_connstring));
		else
			node->upstream_connstring = NULL;

		res = lappend(res, node);

		MemoryContextSwitchTo(oldcxt);
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	CommitTransactionCommand();

	return res;
}

/*
 * Internal routine for updaing the status of the node.
 */
static void
update_node_status_internal(const char *node_id, const char *node_name, LRG_NODE_STATE state, bool is_in_txn)
{
	Relation	rel;
	bool		nulls[Natts_pg_lrg_nodes];
	bool		replaces[Natts_pg_lrg_nodes];
	Datum		values[Natts_pg_lrg_nodes];
	HeapTuple	tup;

	Assert(!(node_id == NULL && node_name == NULL)
			&& !(node_id != NULL && node_name != NULL));

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	values[Anum_pg_lrg_nodes_status - 1] = Int32GetDatum(state);
	replaces[Anum_pg_lrg_nodes_status - 1] = true;

	if (!is_in_txn)
		StartTransactionCommand();

	rel = table_open(LrgNodesRelationId, RowExclusiveLock);

	if (node_id != NULL)
		tup = SearchSysCacheCopy1(LRGNODEID, CStringGetTextDatum(node_id));
	else
		tup = SearchSysCacheCopy1(LRGNODENAME, CStringGetDatum(node_name));

	Assert(HeapTupleIsValid(tup));

	tup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
							replaces);
	CatalogTupleUpdate(rel, &tup->t_self, tup);
	heap_freetuple(tup);

	table_close(rel, NoLock);

	if (!is_in_txn)
		CommitTransactionCommand();
}

/*
 * Update the status of node, that is speciefied by the name
 */
void
update_node_status_by_nodename(const char *node_name, LRG_NODE_STATE state, bool is_in_txn)
{
	update_node_status_internal(NULL, node_name, state, is_in_txn);
}

/*
 * Same as above, but node_id is used for the key
 */
void
update_node_status_by_nodeid(const char *node_id, LRG_NODE_STATE state, bool is_in_txn)
{
	update_node_status_internal(node_id, NULL, state, is_in_txn);
}

/*
 * Helper function for create_publication()
 */
static Oid
find_publication(const char *pubname)
{
	Relation rel;
	HeapTuple tup;
	Form_pg_publication pubform;

	rel = table_open(PublicationRelationId, RowExclusiveLock);

	/* Check if name is used */
	tup = SearchSysCacheCopy1(PUBLICATIONNAME,
							  CStringGetDatum(pubname));

	if (!HeapTupleIsValid(tup))
	{
		table_close(rel, NoLock);
		return InvalidOid;
	}

	pubform = (Form_pg_publication) GETSTRUCT(tup);
	table_close(rel, NoLock);

	return pubform->oid;
}

/*
 * Create publication via SPI interface, and insert its oid
 * to the system catalog pg_lrg_pub.
 */
static void
create_publication(const char* group_name, const char* node_id, Oid group_oid)
{
	int ret;
	StringInfoData query, pub_name;
	Oid pub_oid;
	Oid lrgpub_oid;
	Relation rel;
	bool		nulls[Natts_pg_lrg_pub];
	Datum		values[Natts_pg_lrg_pub];
	HeapTuple tup;

	initStringInfo(&query);
	initStringInfo(&pub_name);

	/* Firstly do CREATE PUBLICATION */
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	appendStringInfo(&pub_name, "pub_for_%s", group_name);
	appendStringInfo(&query, "CREATE PUBLICATION %s %s", pub_name.data, "FOR ALL TABLES");

	ret = SPI_execute(query.data, false, 0);
	if (ret != SPI_OK_UTILITY)
		ereport(ERROR,
				errmsg("could not create a publication"),
				errdetail("Query: %s", query.data));

	pub_oid = find_publication(pub_name.data);
	if (pub_oid == InvalidOid)
		ereport(ERROR,
				errmsg("could not find a publication: %s", pub_name.data));

	rel = table_open(LrgPublicationId, ExclusiveLock);

	memset(nulls, 0, sizeof(nulls));
	memset(values, 0, sizeof(values));

	lrgpub_oid = GetNewOidWithIndex(rel, LrgPublicationOidIndexId, Anum_pg_lrg_pub_oid);

	values[Anum_pg_lrg_pub_oid - 1] = ObjectIdGetDatum(lrgpub_oid);
	values[Anum_pg_lrg_pub_groupid - 1] = ObjectIdGetDatum(group_oid);
	values[Anum_pg_lrg_pub_pubid - 1] = ObjectIdGetDatum(pub_oid);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	/* Insert tuple into catalog. */
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, ExclusiveLock);

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();

	pfree(pub_name.data);
	pfree(query.data);
}

/*
 * Some work for detaching and dropping
 */
static void
detach_node(LrgNode *tobedetached, bool force_detach)
{
	PGconn *tobedetachedconn = NULL;
	List *list;
	ListCell   *lc;
	MemoryContext subctx;
	MemoryContext oldctx;
	char *group_name = NULL;
	bool could_connect;

	get_group_info(&group_name);

	/*
	 * load a library if LRG worker has not used yet
	 */
	if (LrgFunctionTypes == NULL)
		load_file("libpqlrg", false);

	/*
	 * Try to connect to the to-be-detached node,
	 * and throw an ERROR if "force" option is not specified.
	 *
	 * Information about the health of the node must be kept
	 * because we must do some special things if it dies.
	 */
	could_connect = lrg_connect(tobedetached->local_connstring, &tobedetachedconn, false);
	if (!force_detach && !could_connect)
		ereport(ERROR,
				errmsg("could not connect to the to-be-detached node"));

	subctx = AllocSetContextCreate(TopMemoryContext,
									"Lrg Launcher list",
									ALLOCSET_DEFAULT_SIZES);
	oldctx = MemoryContextSwitchTo(subctx);

	list = get_lrg_nodes_list(tobedetached->node_id);

	if (list != NIL)
	{
		foreach(lc, list)
		{
			LrgNode *other_node = (LrgNode *)lfirst(lc);
			PGconn *otherconn = NULL;
			lrg_connect(other_node->local_connstring, &otherconn, true);

			lrg_drop_subscription(group_name, tobedetached->node_id, other_node->node_id,
								  otherconn, !could_connect);
			/*
			 * DROP SUBSCRIPTION in to-be-detached node if possible
			 */
			if (could_connect)
				lrg_drop_subscription(group_name, other_node->node_id, tobedetached->node_id,
									  tobedetachedconn, false);

			lrg_delete_from_nodes(otherconn, tobedetached->node_id);
			lrg_disconnect(otherconn);
		}
	}
	else
	{
		/*
		 * lrg_drop() case. Just delete all tuples from LRG catalogs.
		 */
		lrg_delete_from_nodes(tobedetachedconn, tobedetached->node_id);
	}

	MemoryContextSwitchTo(oldctx);
	MemoryContextDelete(subctx);


	/*
	 * Do garbage collection if we can connect to
	 */
	if (could_connect)
	{
		lrg_drop_publication(group_name, tobedetachedconn);
		lrg_cleanup(tobedetachedconn);
		lrg_disconnect(tobedetachedconn);
	}
	pfree(group_name);
}

/*
 * advance the state machine for creating/attaching
 */
static void
advance_state_machine(LrgNode *local_node, LRG_NODE_STATE initial_status)
{
	PGconn *localconn = NULL;
	PGconn *upstreamconn = NULL;
	char *group_name = NULL;
	LRG_NODE_STATE state = initial_status;
	char node_id[64];

	/*
	 * Assuming that the specified node is local
	 */
	construct_node_id(node_id, sizeof(node_id));
	Assert(strcmp(node_id, local_node->node_id) == 0);

	/*
	 * XXX: Global lock should be acquired around here.
	 */

	if (state == LRG_STATE_INIT)
	{
		/* Establish connection if we are in the attaching case */
		if (local_node->upstream_connstring != NULL)
		{
			load_file("libpqlrg", false);
			lrg_connect(local_node->upstream_connstring, &upstreamconn, true);
			lrg_connect(local_node->local_connstring, &localconn, true);

			/* and get pg_lrg_nodes from upstream */
			synchronise_system_tables(localconn, upstreamconn);
		}
		get_group_info(&group_name);

		create_publication(group_name, local_node->node_id, local_node->group_oid);

		state = LRG_STATE_CREATE_PUBLICATION;
		update_node_status_by_nodename(local_node->node_name, LRG_STATE_CREATE_PUBLICATION, false);
	}

	/*
	 * XXX: We should ensure all changes have been sent to nodes here.
	 */

	if (state == LRG_STATE_CREATE_PUBLICATION)
	{
		if (local_node->upstream_connstring != NULL)
		{
			List *list;
			ListCell   *lc;
			MemoryContext subctx;
			MemoryContext oldctx;

			subctx = AllocSetContextCreate(TopMemoryContext,
											"Lrg Launcher list",
											ALLOCSET_DEFAULT_SIZES);
			oldctx = MemoryContextSwitchTo(subctx);

			/* Get a node list that belong to the group */
			list = get_lrg_nodes_list(local_node->node_id);

			/* and do CREATE SUBSCRIPTION on all nodes! */
			foreach(lc, list)
			{
				LrgNode *other_node = (LrgNode *)lfirst(lc);
				PGconn *otherconn = NULL;
				lrg_connect(other_node->local_connstring, &otherconn, true);

				/*
				 * XXX: Initial data should be synchronized from upstream node,
				 * so a subscription that subscribes upstream node should be set as copy_data = force.
				 */

				lrg_create_subscription(group_name, local_node->local_connstring,
										local_node->node_id, other_node->node_id,
										otherconn, "origin = local, copy_data = false");
				lrg_create_subscription(group_name, other_node->local_connstring,
										other_node->node_id, local_node->node_id,
										localconn, "origin = local, copy_data = false");

				/*
				 * XXX: adding a tuple into remote's pg_lrg_nodes here,
				 * but it is bad. it should be end of this function.
				 */
				if (local_node->upstream_connstring != NULL)
					lrg_insert_into_lrg_nodes(otherconn, local_node->node_id,
							LRG_STATE_READY, local_node->node_name,
							local_node->local_connstring, local_node->upstream_connstring);
				lrg_disconnect(otherconn);
			}
			MemoryContextSwitchTo(oldctx);
			MemoryContextDelete(subctx);
		}

	/*
	 * XXX: Global lock should be released here
	 */

		state = LRG_STATE_CREATE_SUBSCRIPTION;
		update_node_status_by_nodename(local_node->node_name, LRG_STATE_CREATE_SUBSCRIPTION, false);
	}

	state = LRG_STATE_READY;
	update_node_status_by_nodename(local_node->node_name, LRG_STATE_READY, false);

	/*
	 * clean up phase
	 */
	if (localconn != NULL)
		lrg_disconnect(localconn);
	if (upstreamconn != NULL)
		lrg_disconnect(upstreamconn);
	if (group_name != NULL)
		pfree(group_name);
}

/*
 * Get node-specific information that status is not ready.
 */
static void
get_node_information(LrgNode **output_node, LRG_NODE_STATE *status)
{
	Relation	rel;
	HeapTuple	tup;
	SysScanDesc scan;
	Snapshot	current;

	StartTransactionCommand();
	current = GetTransactionSnapshot();

	rel = table_open(LrgNodesRelationId, AccessShareLock);

	scan = systable_beginscan(rel, InvalidOid, false, current, 0, NULL);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		MemoryContext oldcxt;
		LrgNode *tmp;
		LRG_NODE_STATE node_status;
		bool isnull;

		Datum	tmp_upstream_connstring;

		node_status = DatumGetInt32(heap_getattr(tup,
												 Anum_pg_lrg_nodes_status,
												 RelationGetDescr(rel),
												 &isnull));

		/*
		 * If the status is ready, we skip it.
		 */
		if (node_status == LRG_STATE_READY)
			continue;

		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		tmp = (LrgNode *)palloc0(sizeof(LrgNode));

		tmp->group_oid = heap_getattr(tup,
									  Anum_pg_lrg_nodes_groupid,
									  RelationGetDescr(rel),
									  &isnull);

		tmp->node_id = pstrdup(TextDatumGetCString(heap_getattr(tup,
															 Anum_pg_lrg_nodes_nodeid,
															 RelationGetDescr(rel),
															 &isnull)));
		tmp->node_name = pstrdup(DatumGetCString(heap_getattr(tup,
															   Anum_pg_lrg_nodes_nodename,
															   RelationGetDescr(rel),
															   &isnull)));
		tmp->local_connstring = pstrdup(TextDatumGetCString(heap_getattr(tup,
																	  Anum_pg_lrg_nodes_localconn,
																	  RelationGetDescr(rel),
																	  &isnull)));


		/*
		 * the temporary valiable is used in order to do null checking
		 */
		tmp_upstream_connstring = heap_getattr(tup,
											   Anum_pg_lrg_nodes_upstreamconn,
											   RelationGetDescr(rel),
											   &isnull);

		if (!isnull)
			tmp->upstream_connstring = pstrdup(TextDatumGetCString(tmp_upstream_connstring));
		else
			tmp->upstream_connstring = NULL;


		*output_node = tmp;
		*status = node_status;

		MemoryContextSwitchTo(oldcxt);
		break;
	}

	systable_endscan(scan);
	table_close(rel, NoLock);
	CommitTransactionCommand();
}

static void
do_node_management(void)
{
	LrgNode *node = NULL;
	LRG_NODE_STATE status;

	/*
	 * read information from pg_lrg_nodes
	 */
	get_node_information(&node, &status);

	if (node == NULL)
	{
		/*
		 * If we rearch here status of nodes are READY,
		 * it means that no operations are needed.
		 */
		return;
	}

	/*
	 * XXX: for simplify the case for detaching/dropping is completely separated
	 * from the creating/attaching.
	 */
	if (status == LRG_STATE_TO_BE_DETACHED
		|| status == LRG_STATE_FORCE_DETACH)
		detach_node(node, status == LRG_STATE_FORCE_DETACH);
	else
	{
		/*
		 * advance the state machine for creating or attaching.
		 */
		advance_state_machine(node, status);
	}

	pfree(node->node_id);
	pfree(node->node_name);
	pfree(node->local_connstring);
	if (node->upstream_connstring != NULL)
		pfree(node->upstream_connstring);
	pfree(node);
}

/*
 * Entry point for lrg worker
 */
void
lrg_worker_main(Datum arg)
{
	int slot = DatumGetInt32(arg);

	/* Establish signal handlers. */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/*
	 * Get information from the controller. The idex
	 * is given as the argument
	 */
	LWLockAcquire(&LrgWorkerCtx->lock, LW_SHARED);
	my_lrg_worker = &LrgWorkerCtx->workers[slot];
	my_lrg_worker->worker_pid = MyProcPid;
	my_lrg_worker->worker_latch = &MyProc->procLatch;
	LWLockRelease(&LrgWorkerCtx->lock);

	before_shmem_exit(lrg_worker_onexit, (Datum) 0);

	/*
	 * Connect to the "allocated" database as superuser.
	 */
	BackgroundWorkerInitializeConnectionByOid(my_lrg_worker->dbid, 0, 0);

	elog(DEBUG3, "per-db worker for %u was launched", my_lrg_worker->dbid);

	/*
	 * The launcher launches the worker without considering
	 * the existence of lrg related data.
	 * So firstly workers must check their catalogs, and exit
	 * if there is no data.
	 * In any cases pg_lrg_info will have tuples if
	 * this node is in a node group, so we reads it.
	 */
	if (get_group_info(NULL) == InvalidOid)
	{
		elog(DEBUG3, "This database %u is not a member of lrg", MyDatabaseId);
		proc_exit(0);
	}

	do_node_management();

	ResetLatch(&MyProc->procLatch);

	/*
	 * Wait for detaching or dropping.
	 */
	for (;;)
	{
		int rc;
		bool is_latch_set = false;

		CHECK_FOR_INTERRUPTS();

#define TEMPORARY_NAP_TIME 180000L
		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   TEMPORARY_NAP_TIME, 0);

		if (rc & WL_LATCH_SET)
		{
			is_latch_set = true;
			ResetLatch(&MyProc->procLatch);
			CHECK_FOR_INTERRUPTS();
		}

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (is_latch_set)
		{
			do_node_management();
			is_latch_set = false;
		}
	}
}
