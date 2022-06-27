/*-------------------------------------------------------------------------
 *
 * pg_lrg_nodes.h
 *	  definition of the "logical replication nodes" system
 *	  catalog (pg_lrg_nodes)
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_lrg_nodes.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LRG_NODES_H
#define PG_LRG_NODES_H

#include "catalog/genbki.h"
#include "catalog/pg_lrg_nodes_d.h"

/* ----------------
 *		pg_lrg_nodes definition.  cpp turns this into
 *		typedef struct FormData_pg_lrg_nodes
 * ----------------
 */
CATALOG(pg_lrg_nodes,8339,LrgNodesRelationId)
{
	Oid			oid;			/* oid */

	Oid			groupid BKI_LOOKUP(pg_lrg_info);
	Oid 		dbid BKI_LOOKUP(pg_database);
	int32		status;
	NameData	nodename BKI_FORCE_NOT_NULL;
#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		nodeid BKI_FORCE_NOT_NULL;	/* name of the logical replication group */
	text		localconn BKI_FORCE_NOT_NULL;
	text		upstreamconn BKI_FORCE_NULL;
#endif
} FormData_pg_lrg_nodes;

/* ----------------
 *		Form_pg_lrg_nodes corresponds to a pointer to a tuple with
 *		the format of pg_lrg_nodes relation.
 * ----------------
 */
typedef FormData_pg_lrg_nodes *Form_pg_lrg_nodes;

DECLARE_UNIQUE_INDEX_PKEY(pg_lrg_nodes_oid_index, 8340, LrgNodesRelationIndexId, on pg_lrg_nodes using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_lrg_node_id_index, 8346, LrgNodeIdIndexId, on pg_lrg_nodes using btree(nodeid text_ops));
DECLARE_UNIQUE_INDEX(pg_lrg_nodes_name_index, 8347, LrgNodeNameIndexId, on pg_lrg_nodes using btree(nodename name_ops));

#endif							/* PG_LRG_NODES_H */
