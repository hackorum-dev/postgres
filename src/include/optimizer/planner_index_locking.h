#pragma once

#include <postgres.h>

#include <nodes/plannodes.h>
#include <optimizer/paths.h>
#include <utils/rel.h>

extern bool FilterIndexes;

// introduces a bitmap that caches which index covers which columns, so that we can filter the
// possible indexes we should add to any RelOptInfo* to only be the ones that have any overlap with
// the requested columns for a relation.

extern TupleDesc GetPgIndexDescriptor(void);
extern HeapTuple ScanPgRelation(Oid targetRelId, bool indexOK, bool force_non_historic);
extern List     *s64_RelationGetIndexBitmapList(Relation relation);
extern Oid       s64_RelationGetBestIndexForIndexOnly(Relation relation, Bitmapset *required);
extern void      s64_add_indexes(PlannerInfo *root);
extern void      s64_add_indexes_for_rel(PlannerInfo *root,
                                         Oid          relationObjectId,
                                         bool         inhparent,
                                         RelOptInfo  *rel);

typedef struct IndexBitmapset
{
    Oid        Index;
    Bitmapset *Keys;
} IndexBitmapset;
extern IndexBitmapset *s64_RelationBuildIndexBitmapset(HeapTuple htup);
extern bool            s64_IsUnnecessaryIndex(PlannerInfo    *root,
                                              IndexBitmapset *bitmap,
                                              Bitmapset      *clauses,
                                              Bitmapset      *all,
                                              Oid             smallestIndex);

extern int        list_bitmapset_qcmp(const void *p1, const void *p2);
extern int        list_bitmapset_cmp(const ListCell *p1, const ListCell *p2);
extern Bitmapset *s64_RelationUsedTList(PlannerInfo *root, RelOptInfo *rel);
extern Bitmapset *s64_RelationUsedClauses(PlannerInfo *root, RelOptInfo *rel);
