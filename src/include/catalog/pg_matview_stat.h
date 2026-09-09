/*-------------------------------------------------------------------------
 *
 * pg_matview_stat.h
 *	  definition of the "materialized view refresh statistics" system
 *	  catalog (pg_matview_stat)
 *
 * pg_matview_stat records, per materialized view, when it was last
 * populated with real data and how many times that has happened.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_matview_stat.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_MATVIEW_STAT_H
#define PG_MATVIEW_STAT_H

#include "catalog/genbki.h"
#include "catalog/pg_matview_stat_d.h"

/* ----------------
 *		pg_matview_stat definition.  cpp turns this into
 *		typedef struct FormData_pg_matview_stat
 * ----------------
 */
BEGIN_CATALOG_STRUCT

CATALOG(pg_matview_stat,9716,MatViewStatRelationId)
{
	Oid			mvrelid BKI_LOOKUP(pg_class);	/* OID of the materialized
												 * view */
	int64		mvrefreshcount BKI_DEFAULT(0);	/* number of times the matview
												 * has been populated with
												 * real data */

	/*
	 * Although mvlastrefresh is a fixed-width type, it is allowed to be NULL
	 * (before the first real refresh).
	 */
#ifdef CATALOG_VARLEN			/* variable-length fields start here */

	timestamptz mvlastrefresh BKI_FORCE_NULL;	/* timestamp of the last
												 * refresh that actually
												 * populated the view, or
												 * NULL.  REFRESH ... WITH NO
												 * DATA does not update this. */
#endif
} FormData_pg_matview_stat;

END_CATALOG_STRUCT

typedef FormData_pg_matview_stat * Form_pg_matview_stat;

DECLARE_UNIQUE_INDEX_PKEY(pg_matview_stat_mvrelid_index, 9718, MatViewStatRelidIndexId, pg_matview_stat, btree(mvrelid oid_ops));

MAKE_SYSCACHE(MATVIEWSTATRELID, pg_matview_stat_mvrelid_index, 16);

#endif							/* PG_MATVIEW_STAT_H */
