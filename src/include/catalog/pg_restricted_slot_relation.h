/*-------------------------------------------------------------------------
 *
 * pg_restricted_slot_relation.h
 *	  conservative relation mappings for restricted logical slots
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_restricted_slot_relation.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_RESTRICTED_SLOT_RELATION_H
#define PG_RESTRICTED_SLOT_RELATION_H

#include "catalog/genbki.h"
#include "catalog/pg_restricted_slot_relation_d.h" /* IWYU pragma: export */

BEGIN_CATALOG_STRUCT

CATALOG(pg_restricted_slot_relation,8050,RestrictedSlotRelationRelationId)
{
	NameData	rsrslotname;
	Oid			rsrrelid BKI_LOOKUP_OPT(pg_class);
	int64		rsrincarnation;
} FormData_pg_restricted_slot_relation;

END_CATALOG_STRUCT

typedef FormData_pg_restricted_slot_relation *Form_pg_restricted_slot_relation;

DECLARE_UNIQUE_INDEX_PKEY(pg_restricted_slot_relation_slot_rel_index, 8051, RestrictedSlotRelationSlotRelIndexId, pg_restricted_slot_relation, btree(rsrslotname name_ops, rsrrelid oid_ops, rsrincarnation int8_ops));
DECLARE_INDEX(pg_restricted_slot_relation_rel_index, 8052, RestrictedSlotRelationRelIndexId, pg_restricted_slot_relation, btree(rsrrelid oid_ops));
DECLARE_INDEX(pg_restricted_slot_relation_slot_index, 8053, RestrictedSlotRelationSlotIndexId, pg_restricted_slot_relation, btree(rsrslotname name_ops));

#endif							/* PG_RESTRICTED_SLOT_RELATION_H */
