/*-------------------------------------------------------------------------
 *
 * pg_lrg_sub.h
 *	  definition of the "logical replication group subscription" system
 *	  catalog (pg_lrg_sub)
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_lrg_sub.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LRG_SUB_H
#define PG_LRG_SUB_H

#include "catalog/genbki.h"
#include "catalog/pg_lrg_sub_d.h"

/* ----------------
 *		pg_lrg_sub definition.  cpp turns this into
 *		typedef struct FormData_pg_lrg_sub
 * ----------------
 */
CATALOG(pg_lrg_sub,8343,LrgSubscriptionId)
{
	Oid			oid;
	Oid 		groupid BKI_LOOKUP(pg_lrg_info);;
	Oid 		subid BKI_LOOKUP(pg_subscription);
} FormData_pg_lrg_sub;

/* ----------------
 *		Form_pg_lrg_sub corresponds to a pointer to a tuple with
 *		the format of pg_lrg_sub relation.
 * ----------------
 */
typedef FormData_pg_lrg_sub *Form_pg_lrg_sub;

DECLARE_UNIQUE_INDEX_PKEY(pg_lrg_sub_oid_index, 8345, LrgSubscriptionOidIndexId, on pg_lrg_sub using btree(oid oid_ops));

#endif							/* PG_LRG_SUB_H */
