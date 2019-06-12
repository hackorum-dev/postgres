/*-------------------------------------------------------------------------
 *
 * pg_publication_rel.h
 *	  definition of the system catalog for mappings between relations and
 *	  publications (pg_publication_rel)
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_publication_rel.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PUBLICATION_REL_H
#define PG_PUBLICATION_REL_H

#include "catalog/genbki.h"
#include "catalog/pg_publication_rel_d.h"

/* ----------------
 *		pg_publication_rel definition.  cpp turns this into
 *		typedef struct FormData_pg_publication_rel
 * ----------------
 */
CATALOG(pg_publication_rel,6106,PublicationRelRelationId)
{
	Oid			oid;			/* oid */
	Oid			prpubid;		/* Oid of the publication */
	Oid			prrelid;		/* Oid of the relation */
};

#endif							/* PG_PUBLICATION_REL_H */
