/*-------------------------------------------------------------------------
 *
 * pg_lrg_info.h
 *	  definition of the "logical replication group publication" system
 *	  catalog (pg_lrg_pub)
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_lrg_pub.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LRG_PUB_H
#define PG_LRG_PUB_H

#include "catalog/genbki.h"
#include "catalog/pg_lrg_pub_d.h"

/* ----------------
 *		pg_lrg_pub definition.  cpp turns this into
 *		typedef struct FormData_pg_lrg_pub
 * ----------------
 */
CATALOG(pg_lrg_pub,8341,LrgPublicationId)
{
	Oid			oid;
	Oid 		groupid BKI_LOOKUP(pg_lrg_info);
	Oid 		pubid BKI_LOOKUP(pg_publication);
} FormData_pg_lrg_pub;

/* ----------------
 *		Form_pg_lrg_pub corresponds to a pointer to a tuple with
 *		the format of pg_lrg_pub relation.
 * ----------------
 */
typedef FormData_pg_lrg_pub *Form_pg_lrg_pub;

DECLARE_UNIQUE_INDEX_PKEY(pg_lrg_pub_oid_index, 8344, LrgPublicationOidIndexId, on pg_lrg_pub using btree(oid oid_ops));

#endif							/* PG_LRG_PUB_H */
