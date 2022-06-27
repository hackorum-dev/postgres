/*-------------------------------------------------------------------------
 *
 * libpqlrg.c
 *
 * This file contains the libpq-specific parts of lrg feature. It's
 * loaded as a dynamic module to avoid linking the main server binary with
 * libpq.
 *-------------------------------------------------------------------------
 */


#include "postgres.h"

#include "access/heapam.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "lib/stringinfo.h"
#include "replication/libpqlrg.h"
#include "replication/lrg.h"
#include "utils/snapmgr.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

/* Prototypes for interface functions */
static bool libpqlrg_connect(const char *connstring, PGconn **conn, bool should_throw_error);
static bool libpqlrg_check_group(PGconn *conn, const char *group_name);
static void libpqlrg_copy_lrg_nodes(PGconn *remoteconn, PGconn *localconn);
static void libpqlrg_insert_into_lrg_nodes(PGconn *remoteconn,
										   const char *node_id, LRG_NODE_STATE status,
										   const char *node_name, const char *local_connstring,
										   const char *upstream_connstring);

static void libpqlrg_create_subscription(const char *group_name, const char *publisher_connstring,
										 const char *publisher_node_id, const char *subscriber_node_id,
										 PGconn *subscriberconn, const char *options);

static void libpqlrg_drop_publication(const char *group_name,
									  PGconn *publisherconn);

static void libpqlrg_drop_subscription(const char *group_name,
										const char *publisher_node_id, const char *subscriber_node_id,
										PGconn *subscriberconn, bool need_to_alter);

static void libpqlrg_delete_from_nodes(PGconn *conn, const char *node_id);

static void libpqlrg_cleanup(PGconn *conn);

static void libpqlrg_disconnect(PGconn *conn);

static lrg_function_types PQLrgFunctionTypes =
{
	libpqlrg_connect,
	libpqlrg_check_group,
	libpqlrg_copy_lrg_nodes,
	libpqlrg_insert_into_lrg_nodes,
	libpqlrg_create_subscription,
	libpqlrg_drop_publication,
	libpqlrg_drop_subscription,
	libpqlrg_delete_from_nodes,
	libpqlrg_cleanup,
	libpqlrg_disconnect
};

/*
 * Executes PQconnectdb() and PQstatus(), and check privilege
 */
static bool
libpqlrg_connect(const char *connstring, PGconn **conn, bool should_throw_error)
{
	PGresult *result;

	*conn = PQconnectdb(connstring);

	/*
	 * If the remote host goes down, throws an ERROR or return immediately.
	 */
	if (PQstatus(*conn) != CONNECTION_OK)
	{
		if (should_throw_error)
			ereport(ERROR,
					errmsg("could not connect to the server"),
					errhint("Please check the connection string and health of destination."));
		else
			return false;
	}

	/*
	 * Ensure the the connection is established as superuser.
	 * Throws an ERROR if not. 
 	 */
	result = PQexec(*conn, "SELECT usesuper FROM pg_user WHERE usename = current_user");
	if (strcmp(PQgetvalue(result, 0, 0), "t") != 0)
	{
		PQfinish(*conn);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser for LRG works")));
	}
	PQclear(result);

	return true;
}


/*
 * Check whether the node is in the specified group or not.
 */
static bool
libpqlrg_check_group(PGconn *conn, const char *group_name)
{
	PGresult *result;
	StringInfoData query;
	bool ret;

	Assert(PQstatus(conn) == CONNECTION_OK);
	initStringInfo(&query);
	appendStringInfo(&query, "SELECT COUNT(*) FROM pg_lrg_info WHERE groupname = '%s'", group_name);

	result = PQexec(conn, query.data);

	ret = atoi(PQgetvalue(result, 0, 0));
	pfree(query.data);

	return ret != 0;
}

/*
 * Copy pg_lrg_nodes from remoteconn.
 */
static void
libpqlrg_copy_lrg_nodes(PGconn *remoteconn, PGconn *localconn)
{
	PGresult *result;
	StringInfoData query;
	int i, num_tuples;

	Assert(PQstatus(remoteconn) == CONNECTION_OK
		   && PQstatus(localconn) == CONNECTION_OK);
	initStringInfo(&query);


	/*
	 * Note that COPY command cannot be used here because group_oid
	 * might be different between remote and local.
	 */
	appendStringInfo(&query, "SELECT nodeid, status, nodename, "
							 "localconn, upstreamconn FROM pg_lrg_nodes");
	result = PQexec(remoteconn, query.data);
	if (PQresultStatus(result) != PGRES_TUPLES_OK)
		ereport(ERROR,
				errmsg("could not read pg_lrg_nodes from the upstream node"),
				errhint("Is the server really running?"));

	resetStringInfo(&query);

	num_tuples = PQntuples(result);

	for(i = 0; i < num_tuples; i++)
	{
		char *node_id;
		char *status;
		char *nodename;
		char *localconn;
		char *upstreamconn;

		node_id = PQgetvalue(result, i, 0);
		status = PQgetvalue(result, i, 1);
		nodename = PQgetvalue(result, i, 2);
		localconn = PQgetvalue(result, i, 3);
		upstreamconn = PQgetvalue(result, i, 4);

		StartTransactionCommand();
		(void) GetTransactionSnapshot();
		/*
		 * group_oid is adjusted to local value
		 */
		lrg_add_nodes(node_id, get_group_info(NULL), atoi(status), nodename, localconn, upstreamconn);
		CommitTransactionCommand();
	}
}

/*
 * Insert data to remote's pg_lrg_nodes. It will be done
 * via internal SQL function.
 */
static void
libpqlrg_insert_into_lrg_nodes(PGconn *remoteconn,
							   const char *node_id, LRG_NODE_STATE status,
							   const char *node_name, const char *local_connstring,
							   const char *upstream_connstring)
{
	StringInfoData query;
	PGresult *result;

	Assert(PQstatus(remoteconn) == CONNECTION_OK
		   && node_id != NULL
		   && node_name != NULL
		   && local_connstring != NULL
		   && upstream_connstring != NULL);

	initStringInfo(&query);
	appendStringInfo(&query, "SELECT lrg_insert_into_nodes('%s', %d, '%s', '%s', '%s')",
					 node_id, status, node_name, local_connstring, upstream_connstring);

	result = PQexec(remoteconn, query.data);
	if (PQresultStatus(result) != PGRES_TUPLES_OK)
		ereport(ERROR,
				errmsg("could not execute lrg_insert_into_nodes on the remote node"),
				errhint("Is the server really running?"));

	PQclear(result);
	pfree(query.data);
}

/*
 * Create a subscription with given name and parameters, and
 * add a tuple to remote's pg_lrg_sub.
 *
 * Note that both of this and  libpqlrg_insert_into_lrg_nodes()
 * must be called during attaching a node.
 */
static void
libpqlrg_create_subscription(const char *group_name, const char *publisher_connstring,
							 const char *publisher_node_id, const char *subscriber_node_id,
							 PGconn *subscriberconn, const char *options)
{
	StringInfoData query, sub_name;
	PGresult *result;

	Assert(publisher_connstring != NULL && subscriberconn != NULL);

	/*
	 * the name of subscriber is just concat of two node_id.
	 */
	initStringInfo(&query);
	initStringInfo(&sub_name);

	/*
	 * construct the name of subscription and query.
	 */
	appendStringInfo(&sub_name, "sub_%s_%s", subscriber_node_id, publisher_node_id);
	appendStringInfo(&query, "CREATE SUBSCRIPTION %s CONNECTION '%s' PUBLICATION pub_for_%s",
					 sub_name.data, publisher_connstring, group_name);

	if (options)
		appendStringInfo(&query, " WITH (%s)", options);

	result = PQexec(subscriberconn, query.data);
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
		ereport(ERROR,
				errmsg("could not create a subscription : %s", sub_name.data),
				errhint("Is the server really running?"));
	PQclear(result);

	resetStringInfo(&query);
	appendStringInfo(&query, "SELECT lrg_insert_into_sub('%s')", sub_name.data);
	result = PQexec(subscriberconn, query.data);
	if (PQresultStatus(result) != PGRES_TUPLES_OK)

	PQclear(result);

	pfree(sub_name.data);
	pfree(query.data);
}

/*
 * Drop a given publication and delete a tuple
 * from remote's pg_lrg_pub.
 */
static void
libpqlrg_drop_publication(const char *group_name,
						  PGconn *publisherconn)
{
	StringInfoData query, pub_name;
	PGresult *result;

	Assert(PQstatus(publisherconn) == CONNECTION_OK);

	initStringInfo(&query);
	initStringInfo(&pub_name);

	appendStringInfo(&pub_name, "pub_for_%s", group_name);
	appendStringInfo(&query, "DROP PUBLICATION %s", pub_name.data);

	result = PQexec(publisherconn, query.data);
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
		ereport(ERROR,
				errmsg("could not drop the publication : %s", pub_name.data),
				errhint("Is the server really running?"));
	PQclear(result);
	pfree(pub_name.data);
	pfree(query.data);
}

/*
 * same as above, but for subscription.
 */
static void
libpqlrg_drop_subscription(const char *group_name,
						   const char *publisher_node_id, const char *subscriber_node_id,
						   PGconn *subscriberconn, bool need_to_alter)
{
	StringInfoData query, sub_name;
	PGresult *result;

	Assert(PQstatus(subscriberconn) == CONNECTION_OK);

	/*
	 * the name of subscriber is just concat of two node_id.
	 */
	initStringInfo(&query);
	initStringInfo(&sub_name);

	/*
	 * construct the name of subscription.
	 */
	appendStringInfo(&sub_name, "sub_%s_%s", subscriber_node_id, publisher_node_id);

	/*
	 * If the publisher node is not reachable, the subscription cannot be dropped
	 * easily. Followings are needed:
	 *
	 * - disable the subscription
	 * - disassociate the subscription from the replication slot
	 */
	if (need_to_alter)
	{
		appendStringInfo(&query, "ALTER SUBSCRIPTION %s DISABLE", sub_name.data);
		result = PQexec(subscriberconn, query.data);
		if (PQresultStatus(result) != PGRES_COMMAND_OK)
			ereport(ERROR,
					errmsg("could not alter the subscription : %s", query.data),
					errhint("Is the server really running?"));
		PQclear(result);
		resetStringInfo(&query);

		appendStringInfo(&query, "ALTER SUBSCRIPTION %s SET (slot_name = NONE)", sub_name.data);
		result = PQexec(subscriberconn, query.data);
		if (PQresultStatus(result) != PGRES_COMMAND_OK)
			ereport(ERROR,
					errmsg("could not alter the subscription : %s", query.data),
					errhint("Is the server really running?"));
		PQclear(result);
		resetStringInfo(&query);
	}

	appendStringInfo(&query, "DROP SUBSCRIPTION %s", sub_name.data);

	result = PQexec(subscriberconn, query.data);
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
		ereport(ERROR,
				errmsg("could not drop the subscription : %s", query.data),
				errhint("Is the server really running?"));
	PQclear(result);

	/*
	 * ...Moreover, we must delete the remaining replication slot.
	 */
	if (need_to_alter)
	{
		resetStringInfo(&sub_name);
		resetStringInfo(&query);

		appendStringInfo(&sub_name, "sub_%s_%s", publisher_node_id, subscriber_node_id);

		appendStringInfo(&query, "SELECT pg_drop_replication_slot('%s')", sub_name.data);
		result = PQexec(subscriberconn, query.data);
		if (PQresultStatus(result) != PGRES_TUPLES_OK)
			ereport(ERROR,
					errmsg("could not drop replication slot : %s", query.data),
					errhint("Is the server really running?"));
		PQclear(result);
	}

	pfree(sub_name.data);
	pfree(query.data);
}

/*
 * Delete data to remote's pg_lrg_nodes. It will be done
 * via internal SQL function.
 */
static void
libpqlrg_delete_from_nodes(PGconn *conn, const char *node_id)
{
	StringInfoData query;
	PGresult *result;

	Assert(PQstatus(conn) == CONNECTION_OK);

	initStringInfo(&query);
	appendStringInfo(&query, "DELETE FROM pg_lrg_nodes WHERE nodeid = '%s'", node_id);

	result = PQexec(conn, query.data);
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
		ereport(ERROR,
				errmsg("could not delete a tuple from remote's pg_lrg_nodes"),
				errhint("Is the server really running?"));
	PQclear(result);
	pfree(query.data);
}

/*
 * Delete all data from LRG catalogs
 */
static void
libpqlrg_cleanup(PGconn *conn)
{
	PGresult *result;
	Assert(PQstatus(conn) == CONNECTION_OK);

	result = PQexec(conn, "DELETE FROM pg_lrg_pub;"
						  "DELETE FROM pg_lrg_sub;"
						  "DELETE FROM pg_lrg_nodes;"
						  "DELETE FROM pg_lrg_info;");
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
		ereport(ERROR,
				errmsg("could not delete data from remote's system catalogs"),
				errhint("Is the server really running?"));

	PQclear(result);
}

/*
 * Just a wrapper for PQfinish()
 */
static void
libpqlrg_disconnect(PGconn *conn)
{
	PQfinish(conn);
}

/*
 * Module initialization function
 */
void
_PG_init(void)
{
	if (LrgFunctionTypes != NULL)
		elog(ERROR, "libpqlrg already loaded");
	LrgFunctionTypes = &PQLrgFunctionTypes;
}
