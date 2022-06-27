/*-------------------------------------------------------------------------
 *
 * pg_lrg_info.h
 *	  definition of the "logical replication group information" system
 *	  catalog (pg_lrg_info)
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_lrg_info.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LRG_INFO_H
#define PG_LRG_INFO_H

#include "catalog/genbki.h"
#include "catalog/pg_lrg_info_d.h"

/* ----------------
 *		pg_lrg_info definition.  cpp turns this into
 *		typedef struct FormData_pg_lrg_info
 * ----------------
 */
CATALOG(pg_lrg_info,8337,LrgInfoRelationId)
{
	Oid			oid;			/* oid */

	NameData	groupname;		/* name of the logical replication group */
#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		pub_type BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_lrg_info;

/* ----------------
 *		Form_pg_lrg_info corresponds to a pointer to a tuple with
 *		the format of pg_lrg_info relation.
 * ----------------
 */
typedef FormData_pg_lrg_info *Form_pg_lrg_info;

DECLARE_UNIQUE_INDEX_PKEY(pg_lrg_info_oid_index, 8338, LrgInfoRelationIndexId, on pg_lrg_info using btree(oid oid_ops));

#endif							/* PG_LRG_INFO_H */
