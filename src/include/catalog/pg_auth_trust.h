/*-------------------------------------------------------------------------
 *
 * pg_auth_trust.h
 *	  definition of the "authorization identifier trust" system catalog
 *	  (pg_auth_trust)
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_auth_trust.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AUTH_TRUST_H
#define PG_AUTH_TRUST_H

#include "catalog/genbki.h"
#include "catalog/pg_auth_trust_d.h"

/* ----------------
 *		pg_auth_trust definition.  cpp turns this into
 *		typedef struct FormData_pg_auth_trust
 * ----------------
 */
CATALOG(pg_auth_trust,1364,AuthTrustRelationId) BKI_SHARED_RELATION
{
	Oid			grantor;		/* trusting role; InvalidOid is a wildcard */
	Oid			trustee;		/* trusted role; InvalidOid is a wildcard */
} FormData_pg_auth_trust;

/* ----------------
 *		Form_pg_auth_trust corresponds to a pointer to a tuple with
 *		the format of pg_auth_trust relation.
 * ----------------
 */
typedef FormData_pg_auth_trust *Form_pg_auth_trust;

#endif   /* PG_AUTH_TRUST_H */
