#include <postgres.h> // NOTE: this include MUST come first

#include <access/htup_details.h>
#include <access/sysattr.h>
#include <catalog/pg_am.h>
#include <catalog/pg_class.h>
#include <nodes/pathnodes.h>
#include <optimizer/optimizer.h>
#include <optimizer/planner_index_locking.h>
#include <utils/builtins.h>
#include <utils/catcache.h>
#include <utils/rel.h>
#include <utils/syscache.h>

bool FilterIndexes = false;

// similar to e.g. RelationGetPrimaryKeyIndex
List *s64_RelationGetIndexBitmapList(Relation relation)
{
    if (!relation->rd_indexvalid)
        list_free(RelationGetIndexList(relation));

    return list_copy(relation->rd_indexprs);
}

IndexBitmapset *s64_RelationBuildIndexBitmapset(HeapTuple htup)
{
    Form_pg_index   index         = (Form_pg_index)GETSTRUCT(htup);
    Bitmapset      *keys          = NULL;
    IndexBitmapset *bitmap        = NULL;
    MemoryContext   oldcxt;
    int             i;

    for (i = 0; i < index->indnatts; ++i)
        if (index->indkey.values[i] != 0)
            keys = bms_add_member(keys,
                                  index->indkey.values[i] - FirstLowInvalidHeapAttributeNumber);

    // Find also all columns referenced by any expression
    // From RelationGetIndexExpressions and inlined so that we don't need extra locks
    if (!heap_attisnull(htup, Anum_pg_index_indexprs, NULL))
    {
        bool  isNull;
        Datum exprsDatum =
                heap_getattr(htup, Anum_pg_index_indexprs, GetPgIndexDescriptor(), &isNull);
        char *exprsString = TextDatumGetCString(exprsDatum);
        Node *exprs       = (Node *)stringToNode(exprsString);
        pull_varattnos(exprs, 1, &keys);
        pfree(exprsString);
        pfree(exprs);
    }

    // Also find any column referenced by any predicate
    if (!heap_attisnull(htup, Anum_pg_index_indpred, NULL))
    {
        bool  isNull;
        Datum predDatum =
                heap_getattr(htup, Anum_pg_index_indpred, GetPgIndexDescriptor(), &isNull);
        char *predString = TextDatumGetCString(predDatum);
        Node *pred       = (Node *)stringToNode(predString);
        pull_varattnos(pred, 1, &keys);
        pfree(predString);
        pfree(pred);
    }

    oldcxt                = MemoryContextSwitchTo(CacheMemoryContext);
    bitmap                = palloc0(sizeof(IndexBitmapset));
    bitmap->Index         = index->indexrelid;
    bitmap->Keys          = bms_copy(keys);
    MemoryContextSwitchTo(oldcxt);
    bms_free(keys);
    return bitmap;
}

// The best index for an index-only scan is the index that:
// - is the smallest
// - has all the fields so that it can actually be an index-only scan.
Oid s64_RelationGetBestIndexForIndexOnly(Relation relation, Bitmapset *required)
{
    int       smallest  = INDEX_MAX_KEYS + 1;
    Oid       bestIndex = InvalidOid;
    ListCell *lc;

    if (!relation->rd_indexvalid)
        list_free(RelationGetIndexList(relation));

    foreach (lc, relation->rd_indexprs)
    {
        IndexBitmapset *bitmap = (IndexBitmapset *)lfirst(lc);

        if (required == NULL || bms_is_subset(required, bitmap->Keys))
        {
            if (bms_num_members(bitmap->Keys) < smallest)
            {
                bestIndex = bitmap->Index;
                smallest  = bms_num_members(bitmap->Keys);
            }
        }
    }

    return bestIndex;
}

int list_bitmapset_qcmp(const void *p1, const void *p2)
{
    return list_bitmapset_cmp(*(ListCell **)p1, *(ListCell **)p2);
}

int list_bitmapset_cmp(const ListCell *p1, const ListCell *p2)
{
    const IndexBitmapset *v1 = (IndexBitmapset *)(lfirst(p1));
    const IndexBitmapset *v2 = (IndexBitmapset *)(lfirst(p2));
    if (v1->Index < v2->Index)
        return -1;
    if (v1->Index > v2->Index)
        return 1;
    return 0;
}

Bitmapset *s64_RelationUsedClauses(PlannerInfo *root, RelOptInfo *rel)
{
    ListCell  *lc1  = NULL;
    ListCell  *lc2  = NULL;
    Bitmapset *used = NULL;

    // All columns we filter on
    foreach (lc1, rel->baserestrictinfo)
    {
        RestrictInfo *rinfo = (RestrictInfo *)lfirst(lc1);
        pull_varattnos((Node *)(rinfo->clause), rel->relid, &used);
    }

    // All columns explicitly used in a join
    foreach (lc1, rel->joininfo)
    {
        RestrictInfo *rinfo = (RestrictInfo *)lfirst(lc1);
        pull_varattnos((Node *)(rinfo->clause), rel->relid, &used);
    }

    // All columns used in any ordering.
    // Query pathkeys contains any useful ordering for query_planner, so we don't have to figure
    // out which ordering would be most useful (grouping, window, distinct, sort, etc).
    foreach (lc1, root->query_pathkeys)
    {
        PathKey *pathKey = (PathKey *)lfirst(lc1);
        foreach (lc2, pathKey->pk_eclass->ec_members)
        {
            EquivalenceMember *ecMember = (EquivalenceMember *)lfirst(lc2);
            if (bms_is_member(rel->relid, ecMember->em_relids))
                pull_varattnos((Node *)(ecMember->em_expr), rel->relid, &used);
        }
    }

    // All useful equivalences there might be, can be used e.g. when you have an index
    // which can return ordered data for a join indirectly (because the data is somehow
    // ordered the same way, e.g. because of a PK)
    foreach (lc1, root->eq_classes)
    {
        EquivalenceClass *ec = (EquivalenceClass *)lfirst(lc1);
        foreach (lc2, ec->ec_members)
        {
            EquivalenceMember *ecMember = (EquivalenceMember *)lfirst(lc2);
            if (bms_is_member(rel->relid, ecMember->em_relids))
                pull_varattnos((Node *)(ecMember->em_expr), rel->relid, &used);
        }
    }

    return used;
}

Bitmapset *s64_RelationUsedTList(PlannerInfo *root, RelOptInfo *rel)
{
    Bitmapset *used = NULL;

    pull_varattnos((Node *)(rel->reltarget->exprs), rel->relid, &used);
    return used;
}

// Any index that cannot provide a reduction of the data read is unnecessary.
// An index can filter out (enough) data when:
// 1. the columns used in the clauses that the index covers can filter
//    out enough rows to not hit all pages of the table
// 2. the index has all fields required to allow an index-only scan,
//    the data is all-visible, and we want actual fields.
// 3. we don't want any fields and this is the smallest index.
//    this is used in a `SELECT COUNT(*) FROM`
// 4. the index can provide the data ordered on a column that is useful
bool s64_IsUnnecessaryIndex(PlannerInfo    *root,
                            IndexBitmapset *bitmap,
                            Bitmapset      *clauses,
                            Bitmapset      *all,
                            Oid             smallestIndex)
{
    if (!FilterIndexes)
        return false;

    // for safety only do this on SELECT statements
    if (root->parse->commandType != CMD_SELECT)
        return false;

    // approximation of case 1 and 4. doesn't (yet) take into account visibility fraction
    if (bms_overlap(clauses, bitmap->Keys))
        return false;

    if (!bms_is_empty(all))
    {
        // case 2.
        if (bms_is_subset(all, bitmap->Keys))
            return false;
    }
    else
    {
        // case 3
        if (bitmap->Index == smallestIndex)
            return false;
    }

    // TestCountIndexesFiltered++;
    return true;
}
