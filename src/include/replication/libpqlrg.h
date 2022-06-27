/*-------------------------------------------------------------------------
 *
 * libpqlrg.h
 *		  Constructs a logical replication group
 *
 *-------------------------------------------------------------------------
 */
#ifndef LIBPQLIG_H
#define LIBPQLIG_H

#include "postgres.h"
#include "libpq-fe.h"
#include "replication/lrg.h"

/* function pointers for libpqlrg */

typedef bool (*libpqlrg_connect_fn) (const char *connstring, PGconn **conn, bool should_throw_error);
typedef bool (*libpqlrg_check_group_fn) (PGconn *conn, const char *group_name);
typedef void (*libpqlrg_copy_lrg_nodes_fn) (PGconn *remoteconn, PGconn *localconn);
typedef void (*libpqlrg_insert_into_lrg_nodes_fn) (PGconn *remoteconn,
												   const char *node_id, LRG_NODE_STATE status,
												   const char *node_name, const char *local_connstring,
												   const char *upstream_connstring);
typedef void (*libpqlrg_create_subscription_fn) (const char *group_name, const char *publisher_connstring,
											  const char *publisher_node_id, const char *subscriber_node_id,
											  PGconn *subscriberconn, const char *options);

typedef void (*libpqlrg_drop_publication_fn) (const char *group_name,
											  PGconn *publisherconn);

typedef void (*libpqlrg_drop_subscription_fn) (const char *group_name,
											   const char *publisher_node_id, const char *subscriber_node_id,
											   PGconn *subscriberconn, bool need_to_alter);

typedef void (*libpqlrg_delete_from_nodes_fn) (PGconn *conn, const char *node_id);
typedef void (*libpqlrg_cleanup_fn) (PGconn *conn);

typedef void (*libpqlrg_disconnect_fn) (PGconn *conn);

typedef struct lrg_function_types
{
	libpqlrg_connect_fn libpqlrg_connect;
	libpqlrg_check_group_fn libpqlrg_check_group;
	libpqlrg_copy_lrg_nodes_fn libpqlrg_copy_lrg_nodes;
	libpqlrg_insert_into_lrg_nodes_fn libpqlrg_insert_into_lrg_nodes;
	libpqlrg_create_subscription_fn libpqlrg_create_subscription;
	libpqlrg_drop_publication_fn libpqlrg_drop_publication;
	libpqlrg_drop_subscription_fn libpqlrg_drop_subscription;
	libpqlrg_delete_from_nodes_fn libpqlrg_delete_from_nodes;
	libpqlrg_cleanup_fn libpqlrg_cleanup;
	libpqlrg_disconnect_fn libpqlrg_disconnect;
} lrg_function_types;

extern PGDLLIMPORT lrg_function_types *LrgFunctionTypes;

#define lrg_connect(connstring, conn, should_throw_error) \
	LrgFunctionTypes->libpqlrg_connect(connstring, conn, should_throw_error)
#define lrg_check_group(conn, group_name) \
	LrgFunctionTypes->libpqlrg_check_group(conn, group_name)
#define lrg_copy_lrg_nodes(remoteconn, localconn) \
	LrgFunctionTypes->libpqlrg_copy_lrg_nodes(remoteconn, localconn)

#define lrg_insert_into_lrg_nodes(remoteconn, \
								  node_id, status, \
								  node_name, local_connstring, \
								  upstream_connstring) \
	LrgFunctionTypes->libpqlrg_insert_into_lrg_nodes(remoteconn, \
													 node_id, status, \
													 node_name, local_connstring, \
													 upstream_connstring)
#define lrg_create_subscription(group_name, publisher_connstring, \
								publisher_node_id, subscriber_node_id, \
								subscriberconn, options) \
	LrgFunctionTypes->libpqlrg_create_subscription(group_name, publisher_connstring, \
												publisher_node_id, subscriber_node_id, \
												subscriberconn, options)

#define lrg_drop_publication(group_name, \
							  publisherconn) \
	LrgFunctionTypes->libpqlrg_drop_publication(group_name, \
												 publisherconn)

#define lrg_drop_subscription(group_name, \
							  publisher_node_id, subscriber_node_id, \
							  subscriberconn, need_to_alter) \
	LrgFunctionTypes->libpqlrg_drop_subscription(group_name, \
												 publisher_node_id, subscriber_node_id, \
												 subscriberconn, need_to_alter)

#define lrg_delete_from_nodes(conn, node_id) \
	LrgFunctionTypes->libpqlrg_delete_from_nodes(conn, node_id)

#define lrg_cleanup(conn) \
	LrgFunctionTypes->libpqlrg_cleanup(conn)

#define lrg_disconnect(conn) \
	LrgFunctionTypes->libpqlrg_disconnect(conn)

#endif /* LIBPQLIG_H */