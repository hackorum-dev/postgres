/*-------------------------------------------------------------------------
 *
 * nodeGraphScan.c
 *	  routines to support native execution of property graph scans (SQL/PGQ)
 *
 * This module implements a lazy, hop-based Depth-First Search (DFS)
 * state machine for executing GRAPH_TABLE clauses.
 *
 * A query like (a)->(b)->(c) is decomposed into hops: (a)->(b) and (b)->(c).
 * Each hop is an iterator that yields (source_vertex, edge, dest_vertex)
 * triples.  Multi-hop is pipelined: for each output of hop 0, hop 1 runs
 * its full inner loop.  Backtracking is natural: when a hop exhausts its
 * edges, we backtrack to the previous hop to try its next destination.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeGraphScan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/genam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/relscan.h"
#include "catalog/pg_propgraph_element.h"
#include "catalog/pg_propgraph_element_label.h"
#include "catalog/pg_propgraph_label_property.h"
#include "executor/executor.h"
#include "executor/nodeGraphScan.h"
#include "executor/nodeSubplan.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "optimizer/cost.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static TupleTableSlot *ExecGraphScan(PlanState *pstate);
static void init_start_vertex(GraphScanState * node);
static bool vle_find_next_edge(GraphScanState * node,
							   Oid src_elemid,
							   int src_nkeys, Datum *src_values,
							   bool *src_nulls,
							   int *start_table_idx, int *start_scan_pass,
							   int edge_elem_idx, EdgeDirection dir,
							   GraphHopFrame * hop,
							   Oid *dest_elemid,
							   int *dest_nkeys, Datum **dest_values,
							   bool **dest_nulls);
static void vle_push_frame(GraphHopFrame * hop, Oid elem_oid, int nkeys,
						   Datum *values, bool *nulls);
static void vle_pop_frame(GraphHopFrame * hop);
static void vle_activate(GraphScanState * node, GraphHopFrame * hop,
						 GraphElementPattern *edge_elem);
static bool is_property_on_all_candidates(GraphScanState * node, int pi,
										  Oid propid);
static bool hop_scan_edges(GraphScanState * node, GraphHopFrame * hop,
						   int edge_elem_idx, EdgeDirection dir);
static void hop_close_edge_scan(GraphHopFrame * hop);
static void hop_reset(GraphHopFrame * hop);
static int	find_vertex_for_edge_dest(GraphScanState * node, int edge_elem_idx);
static int	find_vertex_for_edge_src(GraphScanState * node, int edge_elem_idx);
static void init_column_mapping(GraphScanState * node, EState *estate);
static bool element_matches_label_expr(GraphScanState * node, int ei,
									   Node *labelexpr);
static void fill_one_slot_attr(GraphScanState * node, TupleTableSlot *slot,
							   int slot_att, int ci);
static bool evaluate_graph_where_clause(Node *clause, TupleTableSlot *elem_slot,
										ExprContext *econtext,
										GraphScanState * node, int elem_idx);
static AttrNumber find_attnum_by_name(TupleTableSlot *slot, const char *name);
static void ExtractTargetVertexKeys(TupleTableSlot *edge_slot,
									GraphElementRelInfo * edge_info,
									EdgeDirection dir, int scan_pass,
									int nkeys, Datum *values, bool *nulls);
static TupleTableSlot *fetch_vertex_by_pk(GraphElementRelInfo * vinfo,
										  Datum *pk_values, bool *pk_nulls,
										  int npk, EState *estate);
static Datum resolve_graph_property(GraphScanState * node, int index,
									Oid propid, TupleTableSlot *slot,
									bool *isnull, bool is_edge);
static bool evaluate_graph_where(GraphScanState * node, Node *clause);
static void check_table_privilege(Oid table_oid);
static void check_column_privilege(Oid table_oid, AttrNumber attnum);
static void check_computed_property_column(Oid table_oid, Oid element_oid,
										   Oid propid);
static void check_graph_privileges(GraphScanState * scanstate);
static void store_start_vertex_identity(GraphScanState * node);
static bool check_samevar_edge_identity(GraphScanState * node, int edge_elem_idx,
										GraphHopFrame * hop);
static bool check_samevar_vertex_identity(GraphScanState * node, int dest_elem_idx,
										  GraphHopFrame * hop);
static int	find_ref_elem_index(Node *node, GraphScanState * scanstate);
static void convert_GraphPropertyRef_to_Var(Node **node_p, int elem_idx,
											GraphScanState * scanstate);

/*
 * Helper function to avoid repeating same line (and make it clearer)
 */
static inline bool
edge_is_incoming(EdgeDirection dir, int scan_pass)
{
	return dir == GRAPH_DIR_INCOMING || (dir == GRAPH_DIR_UNDIRECTED && scan_pass == 1);
}

/*
 * Helper: return the pattern element at the given index in the first
 * path pattern.
 */
static inline GraphElementPattern *
GetPatternElement(GraphScanState * node, int index)
{
	GraphScan  *scan = (GraphScan *) node->ss.ps.plan;
	List	   *path;

	if (scan->graph_pattern == NULL ||
		scan->graph_pattern->path_pattern_list == NIL)
		return NULL;

	path = linitial(scan->graph_pattern->path_pattern_list);
	return (GraphElementPattern *) list_nth(path, index);
}

/*
 * Check that the current user has SELECT privilege on the given table.
 * Called before accessing a table at runtime.  Only checks table-level
 * SELECT; column-level checks are done separately when resolving
 * specific properties.
 */
static void
check_table_privilege(Oid table_oid)
{
	if (pg_class_aclcheck(table_oid, GetUserId(),
						  ACL_SELECT) != ACLCHECK_OK)
	{
		char	   *relname = get_rel_name(table_oid);

		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE,
					   relname ? relname : "(unknown)");
	}
}

/*
 * Check that the current user has column-level SELECT privilege on
 * the given column of a backing table.  Only call this when the user
 * does NOT have table-level SELECT on the table.
 */
static void
check_column_privilege(Oid table_oid, AttrNumber attnum)
{
	if (pg_attribute_aclcheck(table_oid, attnum, GetUserId(),
							  ACL_SELECT) != ACLCHECK_OK)
	{
		char	   *relname = get_rel_name(table_oid);

		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE,
					   relname ? relname : "(unknown)");
	}
}

/*
 * At init time, for a computed property that maps a physical column
 * to a property alias (e.g. "three AS lttck"), look up the
 * pg_propgraph_label_property catalog to find the underlying column
 * reference and check column-level SELECT privilege.
 *
 * Only called when the user does NOT have table-level SELECT on the
 * backing table.  The element_oid identifies which element's label
 * the property belongs to.
 */
static void
check_computed_property_column(Oid table_oid, Oid element_oid,
							   Oid propid)
{
	Relation	pl_rel;
	SysScanDesc pl_scan;
	HeapTuple	pl_tup;

	pl_rel = table_open(PropgraphLabelPropertyRelationId, AccessShareLock);
	pl_scan = systable_beginscan(pl_rel, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(pl_tup = systable_getnext(pl_scan)))
	{
		Form_pg_propgraph_label_property pl_form =
			(Form_pg_propgraph_label_property) GETSTRUCT(pl_tup);

		if (pl_form->plppropid != propid)
			continue;

		/*
		 * Found a label-property binding for our property. Check if the label
		 * belongs to our element.
		 */
		{
			Relation	el_rel;
			SysScanDesc el_scan;
			HeapTuple	el_tup;

			el_rel = table_open(PropgraphElementLabelRelationId,
								AccessShareLock);
			el_scan = systable_beginscan(el_rel, InvalidOid, false,
										 NULL, 0, NULL);
			while (HeapTupleIsValid(el_tup = systable_getnext(el_scan)))
			{
				Form_pg_propgraph_element_label el_form =
					(Form_pg_propgraph_element_label) GETSTRUCT(el_tup);

				if (el_form->pgelelid == element_oid &&
					el_form->oid == pl_form->plpellabelid)
				{
					/*
					 * This label belongs to our element.  Extract the
					 * expression and find any Var reference to a physical
					 * column.
					 */
					Datum		exprdatum;
					bool		exprnull;
					Node	   *exprnode;

					exprdatum = heap_getattr(pl_tup,
											 Anum_pg_propgraph_label_property_plpexpr,
											 RelationGetDescr(pl_rel), &exprnull);
					if (!exprnull)
					{
						exprnode = stringToNode(
												TextDatumGetCString(exprdatum));
						if (IsA(exprnode, Var))
						{
							Var		   *var = (Var *) exprnode;

							check_column_privilege(table_oid,
												   var->varattno);
						}
					}
					systable_endscan(el_scan);
					table_close(el_rel, AccessShareLock);
					systable_endscan(pl_scan);
					table_close(pl_rel, AccessShareLock);
					return;
				}
			}
			systable_endscan(el_scan);
			table_close(el_rel, AccessShareLock);
		}
	}
	systable_endscan(pl_scan);
	table_close(pl_rel, AccessShareLock);
}

/*
 * Check table-level and column-level SELECT privilege on all backing
 * tables that match pattern elements in the first path pattern.
 *
 * Runs at init time, before any row processing, so that LIMIT 0 and
 * other short-circuit optimizations don't bypass the privilege check.
 */
static void
check_graph_privileges(GraphScanState * scanstate)
{
	int			pi;

	for (pi = 0; pi < scanstate->num_pi; pi++)
	{
		GraphElementPattern *elem = GetPatternElement(scanstate, pi);
		int			ti;

		if (elem == NULL)
			continue;

		for (ti = 0; ti < scanstate->num_elements; ti++)
		{
			TupleDesc	desc;
			int			col;

			if (!element_matches_label_expr(scanstate, ti,
											elem->labelexpr))
				continue;

			/* Table-level SELECT granted, access to all columns */
			if (pg_class_aclcheck(
								  scanstate->element_info[ti].table_oid,
								  GetUserId(),
								  ACL_SELECT) == ACLCHECK_OK)
				continue;

			desc = scanstate->element_info[ti].tupdesc;

			/* Check column-level SELECT on all key columns */
			for (int ki = 0; ki < scanstate->element_info[ti].nkey_attnums; ki++)
			{
				if (pg_attribute_aclcheck(
										  scanstate->element_info[ti].table_oid,
										  scanstate->element_info[ti].key_attnums[ki],
										  GetUserId(),
										  ACL_SELECT) != ACLCHECK_OK)
				{
					char	   *relname = get_rel_name(
													   scanstate->element_info[ti].table_oid);

					aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE,
								   relname ? relname : "(unknown)");
				}
			}

			/* Check column-level SELECT on each COLUMNS property */
			for (col = 0; col < scanstate->ncols; col++)
			{
				AttrNumber	attnum;
				int			j;

				if (scanstate->col_is_const[col] ||
					scanstate->col_propname[col] == NULL)
					continue;
				if (scanstate->col_elem_index[col] != pi)
					continue;

				/* Try matching the property name as a physical column */
				attnum = InvalidAttrNumber;
				for (j = 0; j < desc->natts; j++)
				{
					Form_pg_attribute attr = TupleDescAttr(desc, j);

					if (!attr->attisdropped &&
						strcmp(NameStr(attr->attname),
							   scanstate->col_propname[col]) == 0)
					{
						attnum = attr->attnum;
						break;
					}
				}

				if (attnum != InvalidAttrNumber)
					check_column_privilege(
										   scanstate->element_info[ti].table_oid, attnum);
				else if (scanstate->col_propid[col] != InvalidOid)
				{
					/*
					 * Aliased computed property (e.g. "three AS lttck"): look
					 * up the catalog to find the underlying column reference.
					 */
					check_computed_property_column(
												   scanstate->element_info[ti].table_oid,
												   scanstate->element_info[ti].element_oid,
												   scanstate->col_propid[col]);
				}
			}
		}
	}
}

/*
 * Update the slot_for_elem cache so that fill_one_slot_attr can read
 * from the correct TupleTableSlot for each pattern element.
 *
 * Pattern element indices: 0=start_vertex, 1=edge0, 2=vertex0,
 * 3=edge1, 4=vertex1, ... etc.
 * For a vertex-only query (n_hops=0), only element 0 is valid.
 */
static void
update_slot_cache(GraphScanState * node)
{
	int			pi = 0;

	/* Element 0 is always the start vertex */
	if (node->start_vertex_slot)
		node->slot_for_elem[0] = node->start_vertex_slot;

	/* For each hop, elements are: edge (2*hi+1), dest vertex (2*hi+2) */
	for (int hi = 0; hi < node->n_hops; hi++)
	{
		GraphHopFrame *hop = &node->hops[hi];

		pi = 2 * hi + 1;
		if (pi < node->num_pi)
			node->slot_for_elem[pi] = hop->edge_slot;

		pi = 2 * hi + 2;
		if (pi < node->num_pi)
		{
			if (hop->to_vertex_slot)
				node->slot_for_elem[pi] = hop->to_vertex_slot;

			/*
			 * VLE zero-hop: no edge was traversed, so to_vertex_slot is not
			 * set.  The destination vertex is the same as the source.
			 */
			else if (hop->vle_active && hop->vle_stack_top == 0)
				node->slot_for_elem[pi] = hop->from_vertex_slot;
		}
	}
}

/*
 * Build the element info array from pg_propgraph_element.
 */
static void
build_element_info(GraphScanState * node)
{
	Relation	pg_rel;
	SysScanDesc scan;
	HeapTuple	tuple;
	int			count = 0;

	pg_rel = table_open(PropgraphElementRelationId, AccessShareLock);
	scan = systable_beginscan(pg_rel, InvalidOid, false, NULL, 0, NULL);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_propgraph_element elem = (Form_pg_propgraph_element) GETSTRUCT(tuple);

		if (elem->pgepgid == node->graph_oid)
			count++;
	}
	systable_endscan(scan);

	node->num_elements = count;
	if (count == 0)
	{
		node->element_info = NULL;
		table_close(pg_rel, AccessShareLock);
		return;
	}

	node->element_info = (GraphElementRelInfo *)
		palloc0(sizeof(GraphElementRelInfo) * count);

	scan = systable_beginscan(pg_rel, InvalidOid, false, NULL, 0, NULL);
	count = 0;
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_propgraph_element elem = (Form_pg_propgraph_element) GETSTRUCT(tuple);

		if (elem->pgepgid != node->graph_oid)
			continue;

		{
			HeapTuple	etup;
			bool		isnull;
			Datum		datum;
			Datum	   *dvalues;
			int			nelems;

			etup = SearchSysCache1(PROPGRAPHELOID,
								   ObjectIdGetDatum(elem->oid));
			if (!HeapTupleIsValid(etup))
				elog(ERROR, "cache lookup failed for propgraph element %u",
					 elem->oid);

			node->element_info[count].element_oid = elem->oid;
			node->element_info[count].table_oid = elem->pgerelid;
			node->element_info[count].heap_rel =
				table_open(elem->pgerelid, AccessShareLock);
			/* Check that the relation supports direct table scans */
			{
				char		relkind = node->element_info[count].heap_rel->
					rd_rel->relkind;

				if (relkind != RELKIND_RELATION &&
					relkind != RELKIND_MATVIEW)
				{
					int			ei;

					/* Close any relations already opened in this batch */
					for (ei = 0; ei < count; ei++)
					{
						if (node->element_info[ei].heap_rel)
						{
							table_close(node->element_info[ei].heap_rel,
										AccessShareLock);
							node->element_info[ei].heap_rel = NULL;
						}
					}
					systable_endscan(scan);
					table_close(pg_rel, AccessShareLock);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot scan relation \"%s\" of type "
									"%c in native graph scan",
									RelationGetRelationName(
															node->element_info[count].heap_rel),
									relkind),
							 errhint("Use SET enable_native_graphtable = off "
									 "for property graphs using views, "
									 "partitioned tables, or foreign "
									 "tables.")));
				}
			}
			node->element_info[count].tupdesc =
				RelationGetDescr(node->element_info[count].heap_rel);
			node->element_info[count].element_kind = elem->pgekind;
			node->element_info[count].nkey_attnums = 0;
			node->element_info[count].key_attnums = NULL;
			node->element_info[count].nsrc_fk_attnums = 0;
			node->element_info[count].src_fk_attnums = NULL;
			node->element_info[count].ndst_fk_attnums = 0;
			node->element_info[count].dst_fk_attnums = NULL;

			/* Read vertex primary key array (pgekey) */
			datum = SysCacheGetAttr(PROPGRAPHELOID, etup,
									Anum_pg_propgraph_element_pgekey,
									&isnull);
			if (!isnull)
			{
				deconstruct_array_builtin(DatumGetArrayTypeP(datum), INT2OID,
										  &dvalues, NULL, &nelems);
				if (nelems > 0)
				{
					int			ki;

					node->element_info[count].nkey_attnums = nelems;
					node->element_info[count].key_attnums = (AttrNumber *)
						palloc(sizeof(AttrNumber) * nelems);
					for (ki = 0; ki < nelems; ki++)
						node->element_info[count].key_attnums[ki] =
							DatumGetInt16(dvalues[ki]);
				}
				pfree(dvalues);
			}

			if (elem->pgekind != 'v')
			{
				/* Read source FK attnum array */
				node->element_info[count].src_vertex_elemid =
					elem->pgesrcvertexid;

				datum = SysCacheGetAttr(PROPGRAPHELOID, etup,
										Anum_pg_propgraph_element_pgesrckey,
										&isnull);
				if (!isnull)
				{
					deconstruct_array_builtin(DatumGetArrayTypeP(datum),
											  INT2OID, &dvalues, NULL, &nelems);
					if (nelems > 0)
					{
						int			ki;

						node->element_info[count].nsrc_fk_attnums = nelems;
						node->element_info[count].src_fk_attnums =
							(AttrNumber *) palloc(sizeof(AttrNumber) * nelems);
						for (ki = 0; ki < nelems; ki++)
							node->element_info[count].src_fk_attnums[ki] =
								DatumGetInt16(dvalues[ki]);
					}
					pfree(dvalues);
				}

				/* Read destination FK attnum array */
				node->element_info[count].dest_vertex_elemid =
					elem->pgedestvertexid;

				datum = SysCacheGetAttr(PROPGRAPHELOID, etup,
										Anum_pg_propgraph_element_pgedestkey,
										&isnull);
				if (!isnull)
				{
					deconstruct_array_builtin(DatumGetArrayTypeP(datum),
											  INT2OID, &dvalues, NULL, &nelems);
					if (nelems > 0)
					{
						int			ki;

						node->element_info[count].ndst_fk_attnums = nelems;
						node->element_info[count].dst_fk_attnums =
							(AttrNumber *) palloc(sizeof(AttrNumber) * nelems);
						for (ki = 0; ki < nelems; ki++)
							node->element_info[count].dst_fk_attnums[ki] =
								DatumGetInt16(dvalues[ki]);
					}
					pfree(dvalues);
				}
			}

			ReleaseSysCache(etup);
		}
		count++;
	}
	systable_endscan(scan);
	table_close(pg_rel, AccessShareLock);

	/* Build label membership for each element */
	{
		Relation	el_rel;
		SysScanDesc el_scan;
		HeapTuple	el_tup;

		el_rel = table_open(PropgraphElementLabelRelationId,
							AccessShareLock);
		el_scan = systable_beginscan(el_rel, InvalidOid, false,
									 NULL, 0, NULL);
		while (HeapTupleIsValid(el_tup = systable_getnext(el_scan)))
		{
			Form_pg_propgraph_element_label el_form =
				(Form_pg_propgraph_element_label) GETSTRUCT(el_tup);
			int			ei;

			for (ei = 0; ei < node->num_elements; ei++)
			{
				if (node->element_info[ei].element_oid ==
					el_form->pgelelid)
				{
					int			nl = node->element_info[ei].nlabels;

					if (nl < 32)
					{
						node->element_info[ei].label_oids[nl] = el_form->pgellabelid;
						node->element_info[ei].nlabels++;
					}
					else
					{
						elog(NOTICE, "Maximum number of labels reached, ignoring label \"%u\"", el_form->pgellabelid);
					}
					break;
				}
			}
		}
		systable_endscan(el_scan);
		table_close(el_rel, AccessShareLock);
	}
}

static void
element_info_cleanup(GraphScanState * node)
{
	int			i;

	for (i = 0; i < node->num_elements; i++)
	{
		if (node->element_info[i].heap_rel)
			table_close(node->element_info[i].heap_rel, AccessShareLock);
	}
}

/*
 * Return the table_index-th matching element info for a given pattern
 * element, considering both element kind and label expression.
 */
static GraphElementRelInfo *
GetElementRelInfo(GraphScanState * node, GraphElementPattern *elem,
				  int table_index)
{
	int			i;
	int			count = 0;

	if (elem == NULL)
		return NULL;

	for (i = 0; i < node->num_elements; i++)
	{
		bool		match = false;

		if (elem->kind == VERTEX_PATTERN &&
			node->element_info[i].element_kind == 'v')
			match = true;
		else if (IS_EDGE_PATTERN(elem->kind) &&
				 node->element_info[i].element_kind == 'e')
			match = true;

		if (!match)
			continue;
		if (!element_matches_label_expr(node, i, elem->labelexpr))
			continue;

		if (count == table_index)
			return &node->element_info[i];
		count++;
	}
	return NULL;
}

/* Forward declarations for property validation helpers */
static void walk_property_refs(Node *node, const char *varname, List **refs);
static bool check_property_on_element(Oid elemoid, Oid propid,
									  Node *labelexpr);
static bool element_matches_label(Oid elemoid, Node *labelexpr);

/*
 * Executor-init validation: verify that every GraphPropertyRef in the
 * scan matches a property defined on at least one candidate backing
 * table.  Runs at executor init time so it catches schema changes
 * between PREPARE and EXECUTE.
 */
static void
validate_graph_property_refs(GraphScan * scan, EState *estate)
{
	Relation	pg_elem;
	SysScanDesc elem_scan;
	HeapTuple	elem_tup;
	RangeTblEntry *rte = exec_rt_fetch(scan->scan.scanrelid, estate);

	if (scan->graph_pattern == NULL ||
		scan->graph_pattern->path_pattern_list == NIL)
		return;

	pg_elem = table_open(PropgraphElementRelationId, AccessShareLock);
	elem_scan = systable_beginscan(pg_elem, InvalidOid, false, NULL, 0, NULL);

	{
		List	   *vertex_oids = NIL;
		List	   *edge_oids = NIL;
		Oid			graph_oid = scan->graph_oid;

		while (HeapTupleIsValid(elem_tup = systable_getnext(elem_scan)))
		{
			Form_pg_propgraph_element elform =
				(Form_pg_propgraph_element) GETSTRUCT(elem_tup);

			if (elform->pgepgid != graph_oid)
				continue;
			if (elform->pgekind == 'v')
				vertex_oids = lappend_oid(vertex_oids, elform->oid);
			else
				edge_oids = lappend_oid(edge_oids, elform->oid);
		}
		systable_endscan(elem_scan);
		table_close(pg_elem, AccessShareLock);

		{
			List	   *path = linitial(scan->graph_pattern->path_pattern_list);
			int			pi;
			int			path_len;

			if (path == NULL)
				return;
			path_len = list_length(path);

			for (pi = 0; pi < path_len; pi++)
			{
				GraphElementPattern *gep = (GraphElementPattern *)
					list_nth(path, pi);
				List	   *candidates = NIL;
				ListCell   *clc;

				if (gep == NULL)
					continue;

				candidates = (IS_EDGE_PATTERN(gep->kind))
					? edge_oids : vertex_oids;

				if (gep->labelexpr != NULL)
				{
					ListCell   *lc;
					List	   *filtered = NIL;

					foreach(lc, candidates)
					{
						if (element_matches_label(lfirst_oid(lc),
												  gep->labelexpr))
							filtered = lappend_oid(filtered,
												   lfirst_oid(lc));
					}
					candidates = filtered;
				}

				if (candidates == NIL)
					continue;

				{
					List	   *prop_refs = NIL;
					ListCell   *prlc;

					if (rte->graph_table_columns)
					{
						ListCell   *lc;

						foreach(lc, rte->graph_table_columns)
						{
							TargetEntry *gte = lfirst_node(TargetEntry, lc);
							Node	   *gcol = (Node *) gte->expr;

							while (IsA(gcol, FuncExpr) ||
								   IsA(gcol, RelabelType) ||
								   IsA(gcol, CoerceViaIO) ||
								   IsA(gcol, CollateExpr))
							{
								if (IsA(gcol, FuncExpr))
									gcol = (Node *) linitial(((FuncExpr *) gcol)->args);
								else if (IsA(gcol, RelabelType))
									gcol = (Node *) ((RelabelType *) gcol)->arg;
								else if (IsA(gcol, CoerceViaIO))
									gcol = (Node *) ((CoerceViaIO *) gcol)->arg;
								else
									gcol = (Node *) ((CollateExpr *) gcol)->arg;
							}

							if (IsA(gcol, GraphPropertyRef))
							{
								GraphPropertyRef *gpr =
									(GraphPropertyRef *) gcol;

								if (gep->variable && gpr->elvarname &&
									strcmp(gep->variable,
										   gpr->elvarname) == 0)
									prop_refs = lappend(prop_refs, gpr);
							}
						}
					}

					if (gep->whereClause)
						walk_property_refs(gep->whereClause,
										   gep->variable, &prop_refs);
					if (gep->subexpr)
					{
						ListCell   *lc;

						foreach(lc, gep->subexpr)
							walk_property_refs((Node *) lfirst(lc),
											   gep->variable, &prop_refs);
					}

					if (scan->graph_pattern->whereClause)
						walk_property_refs(
										   scan->graph_pattern->whereClause,
										   gep->variable, &prop_refs);

					foreach(prlc, prop_refs)
					{
						GraphPropertyRef *gpr =
							(GraphPropertyRef *) lfirst(prlc);
						bool		found_in_any = false;

						foreach(clc, candidates)
						{
							if (check_property_on_element(
														  lfirst_oid(clc), gpr->propid,
														  gep->labelexpr))
							{
								found_in_any = true;
								break;
							}
						}

						if (!found_in_any)
						{
							const char *propname =
								get_propgraph_property_name(gpr->propid);

							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_COLUMN),
									 errmsg("property \"%s\" for "
											"element variable \"%s\" "
											"not found",
											propname, gep->variable)));
						}
					}
				}
			}
		}
	}
}

static void
walk_property_refs(Node *node, const char *varname, List **refs)
{
	if (node == NULL)
		return;

	if (IsA(node, GraphPropertyRef))
	{
		GraphPropertyRef *gpr = (GraphPropertyRef *) node;

		if (gpr->elvarname && varname &&
			strcmp(gpr->elvarname, varname) == 0)
			*refs = lappend(*refs, gpr);
		return;
	}

	if (IsA(node, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) node;
		ListCell   *lc;

		foreach(lc, op->args)
			walk_property_refs((Node *) lfirst(lc), varname, refs);
	}
	else if (IsA(node, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) node;
		ListCell   *lc;

		foreach(lc, b->args)
			walk_property_refs((Node *) lfirst(lc), varname, refs);
	}
	else if (IsA(node, RelabelType))
		walk_property_refs((Node *) ((RelabelType *) node)->arg,
						   varname, refs);
	else if (IsA(node, CoerceViaIO))
		walk_property_refs((Node *) ((CoerceViaIO *) node)->arg,
						   varname, refs);
	else if (IsA(node, FuncExpr))
	{
		FuncExpr   *f = (FuncExpr *) node;
		ListCell   *lc;

		foreach(lc, f->args)
			walk_property_refs((Node *) lfirst(lc), varname, refs);
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *s = (ScalarArrayOpExpr *) node;
		ListCell   *lc;

		foreach(lc, s->args)
			walk_property_refs((Node *) lfirst(lc), varname, refs);
	}
	else if (IsA(node, NullIfExpr))
	{
		NullIfExpr *n = (NullIfExpr *) node;
		ListCell   *lc;

		foreach(lc, n->args)
			walk_property_refs((Node *) lfirst(lc), varname, refs);
	}
}

static bool
check_property_on_element(Oid elemoid, Oid propid,
						  Node *labelexpr)
{
	Relation	pl_rel;
	SysScanDesc pl_scan;
	HeapTuple	pl_tup;

	{
		List	   *label_oids = NIL;

		if (labelexpr != NULL)
		{
			if (IsA(labelexpr, GraphLabelRef))
			{
				GraphLabelRef *lref = (GraphLabelRef *) labelexpr;

				label_oids = lappend_oid(label_oids, lref->labelid);
			}
			else if (IsA(labelexpr, BoolExpr))
			{
				BoolExpr   *b = (BoolExpr *) labelexpr;
				ListCell   *lc;

				foreach(lc, b->args)
				{
					Node	   *arg = (Node *) lfirst(lc);

					if (IsA(arg, GraphLabelRef))
						label_oids = lappend_oid(label_oids,
												 ((GraphLabelRef *) arg)->labelid);
				}
			}
		}

		pl_rel = table_open(PropgraphLabelPropertyRelationId,
							AccessShareLock);
		pl_scan = systable_beginscan(pl_rel, InvalidOid, false,
									 NULL, 0, NULL);
		while (HeapTupleIsValid(pl_tup = systable_getnext(pl_scan)))
		{
			Form_pg_propgraph_label_property pl_form =
				(Form_pg_propgraph_label_property) GETSTRUCT(pl_tup);

			if (pl_form->plppropid != propid)
				continue;

			{
				Relation	el_rel;
				SysScanDesc el_scan;
				HeapTuple	el_tup;

				el_rel = table_open(PropgraphElementLabelRelationId,
									AccessShareLock);
				el_scan = systable_beginscan(el_rel, InvalidOid, false,
											 NULL, 0, NULL);
				while (HeapTupleIsValid(el_tup =
										systable_getnext(el_scan)))
				{
					Form_pg_propgraph_element_label el_form =
						(Form_pg_propgraph_element_label)
						GETSTRUCT(el_tup);

					if (el_form->pgelelid != elemoid ||
						el_form->oid != pl_form->plpellabelid)
						continue;

					if (label_oids == NIL ||
						list_member_oid(label_oids,
										el_form->pgellabelid))
					{
						systable_endscan(el_scan);
						table_close(el_rel, AccessShareLock);
						systable_endscan(pl_scan);
						table_close(pl_rel, AccessShareLock);
						return true;
					}
				}
				systable_endscan(el_scan);
				table_close(el_rel, AccessShareLock);
			}
		}
		systable_endscan(pl_scan);
		table_close(pl_rel, AccessShareLock);
	}
	return false;
}

static bool
element_matches_label(Oid elemoid, Node *labelexpr)
{
	if (labelexpr == NULL)
		return true;

	if (IsA(labelexpr, GraphLabelRef))
	{
		GraphLabelRef *lref = (GraphLabelRef *) labelexpr;
		Relation	el_rel;
		SysScanDesc el_scan;
		HeapTuple	el_tup;

		el_rel = table_open(PropgraphElementLabelRelationId,
							AccessShareLock);
		el_scan = systable_beginscan(el_rel, InvalidOid, false,
									 NULL, 0, NULL);
		while (HeapTupleIsValid(el_tup = systable_getnext(el_scan)))
		{
			Form_pg_propgraph_element_label el_form =
				(Form_pg_propgraph_element_label) GETSTRUCT(el_tup);

			if (el_form->pgelelid == elemoid &&
				el_form->pgellabelid == lref->labelid)
			{
				systable_endscan(el_scan);
				table_close(el_rel, AccessShareLock);
				return true;
			}
		}
		systable_endscan(el_scan);
		table_close(el_rel, AccessShareLock);
		return false;
	}

	if (IsA(labelexpr, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) labelexpr;
		ListCell   *lc;

		foreach(lc, b->args)
		{
			if (element_matches_label(elemoid,
									  (Node *) lfirst(lc)))
				return true;
		}
		return false;
	}

	return true;
}

/* ----------------------------------------------------------------
 *	 ExecInitGraphScan
 * ----------------------------------------------------------------
 */
GraphScanState *
ExecInitGraphScan(GraphScan * node, EState *estate, int eflags)
{
	GraphScanState *scanstate;
	int			nelems;
	int			i;
	List	   *path_elements;

	scanstate = makeNode(GraphScanState);

	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecGraphScan;

	scanstate->graph_oid = node->graph_oid;
	scanstate->path_mode = (PathMode) node->path_mode;
	scanstate->search_algo = (SearchAlgorithm) node->search_algo;

	scanstate->start_vertex_slot = NULL;
	scanstate->start_scan = NULL;
	scanstate->n_start_keys = 0;
	scanstate->start_vertex_values = NULL;
	scanstate->start_vertex_nulls = NULL;
	scanstate->start_table_index = 0;
	scanstate->hops = NULL;
	scanstate->n_hops = 0;
	scanstate->current_hop = -1;
	scanstate->is_initialized = false;
	scanstate->scan_attempts = 0;

	/* Count pattern elements from the first path pattern */
	scanstate->n_pattern_elems = 1;
	scanstate->num_pi = 1;
	if (node->graph_pattern != NULL)
	{
		List	   *ppl = node->graph_pattern->path_pattern_list;

		if (ppl != NIL)
		{
			scanstate->n_pattern_elems =
				list_length(linitial(ppl));
			scanstate->num_pi = scanstate->n_pattern_elems;
		}
	}

	/* Compute number of hops: (N_pattern_elems - 1) / 2 */
	{
		int			pi = scanstate->num_pi;

		if (pi >= 3)
			scanstate->n_hops = (pi - 1) / 2;
		else
			scanstate->n_hops = 0;
	}

	/* Initialize expression context and result tuple slot */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * Share the executor's param values so that Param(PARAM_EXEC) and
	 * Param(PARAM_EXTERN) nodes in element WHERE clauses can be resolved
	 * (e.g., when a plpgsql function passes parameters to a graph query
	 * within it).
	 */
	scanstate->ss.ps.ps_ExprContext->ecxt_param_exec_vals =
		estate->es_param_exec_vals;
	scanstate->ss.ps.ps_ExprContext->ecxt_param_list_info =
		estate->es_param_list_info;

	ExecInitResultTupleSlotTL(&scanstate->ss.ps, &TTSOpsVirtual);

	/* Initialize qual */
	scanstate->ss.ps.qual = ExecInitQual(node->scan.plan.qual,
										 (PlanState *) scanstate);

	/*
	 * Create scan tuple slot with a TupleDesc matching only the graph columns
	 * (graph_table_columns), not the full plan targetlist. The scan slot is
	 * used when projection is active: Var references with varno=scanrelid
	 * read from this slot, while Const entries are evaluated directly by
	 * ExecProject.
	 */
	{
		RangeTblEntry *scanrte = exec_rt_fetch(node->scan.scanrelid,
											   estate);
		TupleDesc	scantupdesc;

		scantupdesc = CreateTemplateTupleDesc(
											  list_length(scanrte->graph_table_columns));
		{
			ListCell   *lc;
			int			ci = 0;

			foreach(lc, scanrte->graph_table_columns)
			{
				TargetEntry *te = (TargetEntry *) lfirst(lc);

				TupleDescInitEntry(scantupdesc, (AttrNumber) (ci + 1),
								   NULL,
								   exprType((Node *) te->expr),
								   exprTypmod((Node *) te->expr),
								   0);
				ci++;
			}
		}

		ExecInitScanTupleSlot(estate, &scanstate->ss,
							  scantupdesc, &TTSOpsVirtual, 0);
	}

	/*
	 * Use ExecConditionalAssignProjectionInfo which checks if the target list
	 * matches the tuple descriptor.  Pass the scan slot's TupleDesc as
	 * inputDesc so that the projection correctly maps Var(varno=scanrelid)
	 * references to the scan slot's attributes.
	 */
	ExecAssignProjectionInfo(&scanstate->ss.ps,
							 scanstate->ss.ss_ScanTupleSlot->
							 tts_tupleDescriptor);

	/* Build element info array.  PG_TRY ensures clean state on error. */
	{
		PG_TRY();
		{
			build_element_info(scanstate);
		}
		PG_CATCH();
		{
			element_info_cleanup(scanstate);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}

	/* Build column mapping for projection */
	init_column_mapping(scanstate, estate);

	/*
	 * Compile column expressions from graph_table_columns for function calls
	 * (upper, coalesce, etc.) that wrap property references.
	 */
	{
		RangeTblEntry *scanrte = exec_rt_fetch(node->scan.scanrelid,
											   estate);
		List	   *gtcols = scanrte->graph_table_columns;
		int			gtcol_count = list_length(gtcols);
		int			ci;

		scanstate->col_exprs = (ExprState **)
			palloc0(sizeof(ExprState *) * scanstate->ncols);
		scanstate->col_expr_elem_index = (int *)
			palloc0(sizeof(int) * scanstate->ncols);

		for (ci = 0; ci < scanstate->ncols; ci++)
		{
			scanstate->col_expr_elem_index[ci] = -1;

			if (ci < gtcol_count)
			{
				TargetEntry *gte = (TargetEntry *)
					list_nth(gtcols, ci);
				Node	   *gcol = (Node *) gte->expr;

				/*
				 * Only compile expressions that are NOT bare GraphPropertyRef
				 * or Const (those are handled by fill_one_slot_attr).
				 */
				if (IsA(gcol, GraphPropertyRef) || IsA(gcol, Const))
					continue;

				/*
				 * Determine which pattern element this column references by
				 * looking at the col_elem_index that init_column_mapping set
				 * for this scan column position.
				 */
				{
					int			cj;

					for (cj = 0; cj < scanstate->ncols; cj++)
					{
						if (scanstate->col_elem_index[cj] >= 0 &&
							cj == ci)
						{
							scanstate->col_expr_elem_index[ci] =
								scanstate->col_elem_index[cj];
							break;
						}
					}

					/*
					 * If col_elem_index isn't set for this column, try
					 * matching by varattno position.
					 */
					if (scanstate->col_expr_elem_index[ci] < 0)
					{
						/* Walk the expression to find GraphPropertyRef */
						scanstate->col_expr_elem_index[ci] =
							find_ref_elem_index(gcol, scanstate);
					}
				}

				if (scanstate->col_expr_elem_index[ci] >= 0)
				{
					/* Replace GraphPropertyRef with Var(OUTER_VAR, attnum) */
					Node	   *expr = copyObject(gcol);

					convert_GraphPropertyRef_to_Var(
													&expr, scanstate->col_expr_elem_index[ci],
													scanstate);

					scanstate->col_exprs[ci] =
						ExecInitExpr((Expr *) expr,
									 (PlanState *) scanstate);
				}
			}
		}
	}

	/* Check all graph privileges at init time (before LIMIT 0 can skip us) */
	check_graph_privileges(scanstate);

	/*
	 * Validate all property references against the backing tables. Run at
	 * executor init time (not plan time) so that schema changes between
	 * PREPARE and EXECUTE are caught.
	 */
	validate_graph_property_refs(node, estate);

	/* Allocate slot_for_elem cache */
	scanstate->slot_for_elem = (TupleTableSlot **)
		palloc0(sizeof(TupleTableSlot *) * scanstate->num_pi);

	/* Create per-query memory context before compiling quals */
	scanstate->graph_cxt = AllocSetContextCreate(CurrentMemoryContext,
												 "GraphScan",
												 ALLOCSET_DEFAULT_SIZES);

	/* Store raw WHERE clause nodes for runtime evaluation */
	{
		path_elements = linitial(
								 node->graph_pattern->path_pattern_list);

		nelems = list_length(path_elements);
		scanstate->element_quals = (ExprState **)
			palloc0(sizeof(ExprState *) * nelems);
		for (i = 0; i < nelems; i++)
		{
			GraphElementPattern *gep = (GraphElementPattern *)
				list_nth(path_elements, i);

			scanstate->element_quals[i] =
				(ExprState *) gep->whereClause;
		}
	}

	/* Store the outer GraphPattern WHERE clause */
	scanstate->where_clause = node->graph_pattern->whereClause;

	/*
	 * Build same-variable groups for pattern elements that share variable
	 * names
	 */
	{
		List	   *pathelems = NIL;

		scanstate->nsamevar_groups = 0;
		scanstate->samevar_group = (int *)
			palloc0(sizeof(int) * scanstate->num_pi);
		{
			int			pi;

			for (pi = 0; pi < scanstate->num_pi; pi++)
				scanstate->samevar_group[pi] = -1;
		}

		if (node->graph_pattern != NULL &&
			node->graph_pattern->path_pattern_list != NIL)
		{
			int			pi,
						pj;

			pathelems = linitial(node->graph_pattern->path_pattern_list);

			for (pi = 0; pi < scanstate->num_pi; pi++)
			{
				GraphElementPattern *gep_i = (GraphElementPattern *)
					list_nth(pathelems, pi);

				if (gep_i == NULL || gep_i->variable == NULL ||
					scanstate->samevar_group[pi] >= 0)
					continue;

				for (pj = pi + 1; pj < scanstate->num_pi; pj++)
				{
					GraphElementPattern *gep_j = (GraphElementPattern *)
						list_nth(pathelems, pj);

					if (gep_j == NULL || gep_j->variable == NULL)
						continue;

					if (strcmp(gep_i->variable, gep_j->variable) != 0)
						continue;

					/* Same variable, assign to group */
					if (scanstate->samevar_group[pi] < 0)
					{
						scanstate->samevar_group[pi] =
							scanstate->nsamevar_groups;
						scanstate->nsamevar_groups++;
					}
					scanstate->samevar_group[pj] =
						scanstate->samevar_group[pi];
				}
			}
		}

		/* Allocate per-group tracking arrays */
		if (scanstate->nsamevar_groups > 0)
		{
			int			ng = scanstate->nsamevar_groups;

			scanstate->samevar_first_pi = (int *)
				palloc0(sizeof(int) * ng);
			scanstate->samevar_elem_kind = (char *)
				palloc0(sizeof(char) * ng);
			scanstate->samevar_nkeys = (int *)
				palloc0(sizeof(int) * ng);
			scanstate->samevar_key_values = (Datum **)
				palloc0(sizeof(Datum *) * ng);
			scanstate->samevar_key_nulls = (bool **)
				palloc0(sizeof(bool *) * ng);
			scanstate->samevar_tid = (ItemPointerData *)
				palloc0(sizeof(ItemPointerData) * ng);

			/* Find first pi for each group and determine element kind */
			{
				int			g;

				for (g = 0; g < ng; g++)
				{
					int			first_pi = -1;
					int			pi;

					for (pi = 0; pi < scanstate->num_pi; pi++)
					{
						if (scanstate->samevar_group[pi] == g)
						{
							if (first_pi < 0)
								first_pi = pi;
						}
					}

					scanstate->samevar_first_pi[g] = first_pi;

					/* Determine element kind from the pattern */
					if (first_pi >= 0 && pathelems != NIL)
					{
						GraphElementPattern *gep = (GraphElementPattern *)
							list_nth(pathelems, first_pi);

						if (gep->kind == VERTEX_PATTERN)
							scanstate->samevar_elem_kind[g] = 'v';
						else
							scanstate->samevar_elem_kind[g] = 'e';
					}
				}
			}
		}
		else
		{
			scanstate->samevar_first_pi = NULL;
			scanstate->samevar_elem_kind = NULL;
			scanstate->samevar_nkeys = NULL;
			scanstate->samevar_key_values = NULL;
			scanstate->samevar_key_nulls = NULL;
			scanstate->samevar_tid = NULL;
		}
	}

	/* Allocate hop frames */
	if (scanstate->n_hops > 0)
	{
		scanstate->hops = (GraphHopFrame *)
			palloc0(sizeof(GraphHopFrame) * scanstate->n_hops);
	}

	return scanstate;
}

/*
 * Evaluate a WHERE clause from a pattern element against a tuple slot.
 * The clause is an analyzed expression tree containing GraphPropertyRef
 * nodes.  Returns true if the clause passes, false if it fails.
 *
 * Handles GraphPropertyRef OP Const comparisons
 * and BoolExpr (AND/OR) combinations.  The operator function is called
 * via DirectFunctionCall2Coll using the OpExpr's opfuncid.
 */

/*
 * evaluate_graph_where_clause
 *
 * Evaluate a WHERE clause (element-level or graph-level) against the
 * current path.  The clause is a raw expression tree with GraphPropertyRef
 * nodes (since rewriting was bypassed).
 *
 * If elem_slot is non-NULL (element-level), GraphPropertyRef is resolved
 * against that slot directly.  If elem_slot is NULL (graph-level), the
 * function walks slot_for_elem[] to find the matching pattern element.
 *
 * Returns true if the clause passes, false if it fails.
 * Returns true if there is no clause.
 */
static bool
evaluate_graph_where_clause(Node *clause, TupleTableSlot *elem_slot,
							ExprContext *econtext,
							GraphScanState * node,
							int elem_idx)
{
	/* Strip any wrapper expressions at the top level */
	while (clause != NULL &&
		   (IsA(clause, RelabelType) || IsA(clause, CollateExpr)))
	{
		if (IsA(clause, RelabelType))
			clause = (Node *) ((RelabelType *) clause)->arg;
		else
			clause = (Node *) ((CollateExpr *) clause)->arg;
	}

	if (clause == NULL)
		return true;

	if (IsA(clause, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) clause;
		Node	   *leftarg = strip_implicit_coercions(linitial(op->args));
		Node	   *rightarg = strip_implicit_coercions(lsecond(op->args));
		Datum		leftval,
					rightval;
		bool		leftnull,
					rightnull;
		Datum		result;

		/* ----- LEFT arg handling ----- */

		if (IsA(leftarg, GraphPropertyRef))
		{
			GraphPropertyRef *gpr = (GraphPropertyRef *) leftarg;
			char	   *propname = get_propgraph_property_name(gpr->propid);

			if (elem_slot != NULL)
			{
				/* Element-level: read from the given slot */
				AttrNumber	attnum = find_attnum_by_name(elem_slot,
														 propname);

				if (attnum != InvalidAttrNumber)
				{
					ExecMaterializeSlot(elem_slot);
					leftval = slot_getattr(elem_slot, attnum, &leftnull);
				}
				else if (node && elem_idx >= 0 &&
						 elem_idx < node->num_pi)
				{
					GraphElementPattern *gep = GetPatternElement(
																 node, elem_idx);

					if (gep && IS_EDGE_PATTERN(gep->kind))
					{
						int			hi = (elem_idx - 1) / 2;

						leftval = resolve_graph_property(node, hi,
														 gpr->propid, elem_slot, &leftnull, true);
					}
					else
						leftval = resolve_graph_property(node, elem_idx,
														 gpr->propid, elem_slot, &leftnull, false);
				}
				else
					return false;
			}
			else
			{
				/* Graph-level: walk slot_for_elem to find the element */
				int			pi;

				leftval = (Datum) 0;
				leftnull = true;
				for (pi = 0; pi < node->num_pi; pi++)
				{
					GraphElementPattern *gep = GetPatternElement(node, pi);

					if (gep && gep->variable &&
						strcmp(gep->variable, gpr->elvarname) == 0 &&
						pi < node->num_pi &&
						node->slot_for_elem[pi] != NULL)
					{
						TupleTableSlot *es = node->slot_for_elem[pi];
						AttrNumber	attnum = find_attnum_by_name(es,
																 propname);

						if (attnum != InvalidAttrNumber)
						{
							ExecMaterializeSlot(es);
							leftval = slot_getattr(es, attnum, &leftnull);
						}
						else
						{
							GraphElementPattern *gep2 =
								GetPatternElement(node, pi);

							if (gep2 && IS_EDGE_PATTERN(gep2->kind))
							{
								int			hi = (pi - 1) / 2;

								leftval = resolve_graph_property(
																 node, hi, gpr->propid, es, &leftnull, true);
							}
							else
								leftval = resolve_graph_property(
																 node, pi, gpr->propid, es, &leftnull, false);
						}
						break;
					}
				}
			}
		}
		else if (IsA(leftarg, Const))
		{
			leftval = ((Const *) leftarg)->constvalue;
			leftnull = ((Const *) leftarg)->constisnull;
		}
		else if (IsA(leftarg, Param))
		{
			Param	   *param = (Param *) leftarg;

			if (param->paramkind == PARAM_EXTERN)
			{
				ParamListInfo paramInfo = econtext ?
					econtext->ecxt_param_list_info : NULL;

				if (paramInfo && param->paramid > 0)
				{
					ParamExternData prmdata;
					ParamExternData *prm;

					if (paramInfo->paramFetch != NULL)
						prm = paramInfo->paramFetch(paramInfo,
													param->paramid,
													false, &prmdata);
					else if (param->paramid <= paramInfo->numParams)
						prm = &paramInfo->params[param->paramid - 1];
					else
						return true;
					leftval = prm->value;
					leftnull = prm->isnull;
				}
				else
					return true;
			}
			else if (param->paramkind == PARAM_EXEC &&
					 econtext && econtext->ecxt_param_exec_vals)
			{
				ParamExecData *prm =
					&econtext->ecxt_param_exec_vals[param->paramid];

				if (prm->execPlan != NULL)
					ExecSetParamPlan((SubPlanState *) prm->execPlan,
									 econtext);
				leftval = prm->value;
				leftnull = prm->isnull;
			}
			else
				return true;
		}
		else if (IsA(leftarg, Var))
		{
			Var		   *var = (Var *) leftarg;

			if (econtext && econtext->ecxt_outertuple &&
				var->varattno > 0 &&
				var->varattno <=
				econtext->ecxt_outertuple->tts_tupleDescriptor->natts)
			{
				ExecMaterializeSlot(econtext->ecxt_outertuple);
				leftval = slot_getattr(econtext->ecxt_outertuple,
									   var->varattno, &leftnull);
			}
			else
				return true;
		}
		else if (IsA(leftarg, RelabelType))
		{
			leftarg = (Node *) ((RelabelType *) leftarg)->arg;
			if (IsA(leftarg, Param))
			{
				Param	   *param = (Param *) leftarg;

				if (param->paramkind == PARAM_EXEC &&
					econtext && econtext->ecxt_param_exec_vals)
				{
					ParamExecData *prm =
						&econtext->ecxt_param_exec_vals[param->paramid];

					if (prm->execPlan != NULL)
						ExecSetParamPlan((SubPlanState *) prm->execPlan,
										 econtext);
					leftval = prm->value;
					leftnull = prm->isnull;
				}
				else
					return true;
			}
			else if (IsA(leftarg, Var))
			{
				Var		   *var = (Var *) leftarg;

				if (econtext && econtext->ecxt_outertuple &&
					var->varattno > 0 &&
					var->varattno <=
					econtext->ecxt_outertuple->tts_tupleDescriptor->natts)
				{
					ExecMaterializeSlot(econtext->ecxt_outertuple);
					leftval = slot_getattr(econtext->ecxt_outertuple,
										   var->varattno, &leftnull);
				}
				else
					return true;
			}
			else
				return true;
		}
		else
			return true;

		/* ----- RIGHT arg handling ----- */

		if (IsA(rightarg, GraphPropertyRef))
		{
			GraphPropertyRef *gpr = (GraphPropertyRef *) rightarg;
			char	   *propname = get_propgraph_property_name(gpr->propid);

			if (elem_slot != NULL)
			{
				AttrNumber	attnum = find_attnum_by_name(elem_slot,
														 propname);

				if (attnum != InvalidAttrNumber)
				{
					ExecMaterializeSlot(elem_slot);
					rightval = slot_getattr(elem_slot, attnum, &rightnull);
				}
				else if (node && elem_idx >= 0 &&
						 elem_idx < node->num_pi)
				{
					GraphElementPattern *gep = GetPatternElement(
																 node, elem_idx);

					if (gep && IS_EDGE_PATTERN(gep->kind))
					{
						int			hi = (elem_idx - 1) / 2;

						rightval = resolve_graph_property(node, hi,
														  gpr->propid, elem_slot, &rightnull, true);
					}
					else
						rightval = resolve_graph_property(node, elem_idx,
														  gpr->propid, elem_slot, &rightnull, false);
				}
				else
					return false;
			}
			else
			{
				int			pi;

				rightval = (Datum) 0;
				rightnull = true;
				for (pi = 0; pi < node->num_pi; pi++)
				{
					GraphElementPattern *gep = GetPatternElement(node, pi);

					if (gep && gep->variable &&
						strcmp(gep->variable, gpr->elvarname) == 0 &&
						pi < node->num_pi &&
						node->slot_for_elem[pi] != NULL)
					{
						TupleTableSlot *es = node->slot_for_elem[pi];
						AttrNumber	attnum = find_attnum_by_name(es,
																 propname);

						if (attnum != InvalidAttrNumber)
						{
							ExecMaterializeSlot(es);
							rightval = slot_getattr(es, attnum, &rightnull);
						}
						else
						{
							GraphElementPattern *gep2 =
								GetPatternElement(node, pi);

							if (gep2 && IS_EDGE_PATTERN(gep2->kind))
							{
								int			hi = (pi - 1) / 2;

								rightval = resolve_graph_property(
																  node, hi, gpr->propid, es, &rightnull, true);
							}
							else
								rightval = resolve_graph_property(
																  node, pi, gpr->propid, es, &rightnull, false);
						}
						break;
					}
				}
			}
		}
		else if (IsA(rightarg, Const))
		{
			rightval = ((Const *) rightarg)->constvalue;
			rightnull = ((Const *) rightarg)->constisnull;
		}
		else if (IsA(rightarg, Param))
		{
			Param	   *param = (Param *) rightarg;

			if (param->paramkind == PARAM_EXTERN)
			{
				ParamListInfo paramInfo = econtext ?
					econtext->ecxt_param_list_info : NULL;

				if (paramInfo && param->paramid > 0)
				{
					ParamExternData prmdata;
					ParamExternData *prm;

					if (paramInfo->paramFetch != NULL)
						prm = paramInfo->paramFetch(paramInfo,
													param->paramid,
													false, &prmdata);
					else
						prm = &paramInfo->params[param->paramid - 1];
					rightval = prm->value;
					rightnull = prm->isnull;
				}
				else
					return true;
			}
			else if (param->paramkind == PARAM_EXEC &&
					 econtext && econtext->ecxt_param_exec_vals)
			{
				ParamExecData *prm =
					&econtext->ecxt_param_exec_vals[param->paramid];

				if (prm->execPlan != NULL)
					ExecSetParamPlan((SubPlanState *) prm->execPlan,
									 econtext);
				rightval = prm->value;
				rightnull = prm->isnull;
			}
			else
				return true;
		}
		else if (IsA(rightarg, Var))
		{
			Var		   *var = (Var *) rightarg;

			if (econtext && econtext->ecxt_outertuple &&
				var->varattno > 0 &&
				var->varattno <=
				econtext->ecxt_outertuple->tts_tupleDescriptor->natts)
			{
				ExecMaterializeSlot(econtext->ecxt_outertuple);
				rightval = slot_getattr(econtext->ecxt_outertuple,
										var->varattno, &rightnull);
			}
			else
				return true;
		}
		else if (IsA(rightarg, RelabelType))
		{
			rightarg = (Node *) ((RelabelType *) rightarg)->arg;
			if (IsA(rightarg, Param))
			{
				Param	   *param = (Param *) rightarg;

				if (param->paramkind == PARAM_EXEC &&
					econtext && econtext->ecxt_param_exec_vals)
				{
					ParamExecData *prm =
						&econtext->ecxt_param_exec_vals[param->paramid];

					if (prm->execPlan != NULL)
						ExecSetParamPlan((SubPlanState *) prm->execPlan,
										 econtext);
					rightval = prm->value;
					rightnull = prm->isnull;
				}
				else
					return true;
			}
			else if (IsA(rightarg, Var))
			{
				Var		   *var = (Var *) rightarg;

				if (econtext && econtext->ecxt_outertuple &&
					var->varattno > 0 &&
					var->varattno <=
					econtext->ecxt_outertuple->tts_tupleDescriptor->natts)
				{
					ExecMaterializeSlot(econtext->ecxt_outertuple);
					rightval = slot_getattr(econtext->ecxt_outertuple,
											var->varattno, &rightnull);
				}
				else
					return true;
			}
			else
				return true;
		}
		else
			return true;

		if (leftnull || rightnull)
			return false;

		{
			FmgrInfo	finfo;

			LOCAL_FCINFO(fcinfo, 2);

			fmgr_info(op->opfuncid, &finfo);
			InitFunctionCallInfoData(*fcinfo, &finfo, 2,
									 op->inputcollid, NULL, NULL);
			fcinfo->args[0].value = leftval;
			fcinfo->args[0].isnull = false;
			fcinfo->args[1].value = rightval;
			fcinfo->args[1].isnull = false;
			result = DatumGetBool(FunctionCallInvoke(fcinfo));
		}
		return result;
	}

	if (IsA(clause, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) clause;
		ListCell   *lc;

		if (b->boolop == AND_EXPR)
		{
			foreach(lc, b->args)
			{
				if (!evaluate_graph_where_clause(
												 (Node *) lfirst(lc), elem_slot, econtext,
												 node, elem_idx))
					return false;
			}
			return true;
		}
		else if (b->boolop == OR_EXPR)
		{
			foreach(lc, b->args)
			{
				if (evaluate_graph_where_clause(
												(Node *) lfirst(lc), elem_slot, econtext,
												node, elem_idx))
					return true;
			}
			return false;
		}
		else if (b->boolop == NOT_EXPR)
		{
			return !evaluate_graph_where_clause(
												(Node *) linitial(b->args), elem_slot, econtext,
												node, elem_idx);
		}
	}

	/* Unknown clause type: pass through (accept) */
	return true;
}

/*
 * evaluate_graph_where
 *
 *		Evaluate the outer GraphPattern WHERE clause against the
 *		current path.  The clause is a raw expression tree with
 *		GraphPropertyRef nodes (since rewriting was bypassed).
 *		Each GraphPropertyRef is resolved against the element slot
 *		based on its elvarname.
 *
 *		Returns true if the clause passes, false if it fails.
 *		Returns true if there is no clause.
 */

static bool
evaluate_graph_where(GraphScanState * node, Node *clause)
{
	return evaluate_graph_where_clause(clause, NULL,
									   node->ss.ps.ps_ExprContext,
									   node, -1);
}

/*
 * Find the attnum for a property name in a slot's TupleDesc.
 */
static AttrNumber
find_attnum_by_name(TupleTableSlot *slot, const char *name)
{
	TupleDesc	desc;
	int			i;

	if (slot == NULL || slot->tts_tupleDescriptor == NULL || name == NULL)
		return InvalidAttrNumber;

	desc = slot->tts_tupleDescriptor;
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		if (!attr->attisdropped && strcmp(NameStr(attr->attname), name) == 0)
			return attr->attnum;
	}
	return InvalidAttrNumber;
}

/*
 * Check whether a catalog element (at index ei in element_info) matches
 * the given label expression.  The label expression is a tree of
 * GraphLabelRef nodes (possibly combined via BoolExpr OR).
 * Returns true if the element has any of the referenced labels.
 */
static bool
element_matches_label_expr(GraphScanState * node, int ei, Node *labelexpr)
{
	GraphElementRelInfo *info;

	if (labelexpr == NULL || ei < 0 || ei >= node->num_elements)
		return true;

	info = &node->element_info[ei];

	if (IsA(labelexpr, GraphLabelRef))
	{
		GraphLabelRef *lref = (GraphLabelRef *) labelexpr;
		int			li;

		for (li = 0; li < info->nlabels; li++)
		{
			if (info->label_oids[li] == lref->labelid)
				return true;
		}
		return false;
	}

	if (IsA(labelexpr, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) labelexpr;
		ListCell   *lc;

		/* OR semantics: any matching label is sufficient */
		foreach(lc, b->args)
		{
			if (element_matches_label_expr(node, ei,
										   (Node *) lfirst(lc)))
				return true;
		}
		return false;
	}

	/* Unknown expression type */
	elog(ERROR, "unsupported label expression node: %d", (int) nodeTag(labelexpr));
	return true;
}

/*
 * Map a pattern position (pi) to a catalog element index by walking
 * the element connectivity graph and applying label filters.
 * Returns -1 if no element found.
 */

/*
 * Tree mutator: recursively replace CollateExpr nodes with their inner
 * argument.  This is needed because ExecInitExprRec doesn't handle
 * CollateExpr, but computed property expressions may contain COLLATE
 * clauses (e.g., 'g2.' || ename COLLATE "C").
 */
static Node *
strip_collate_mutator(Node *node)
{
	if (node == NULL)
		return NULL;

	if (IsA(node, CollateExpr))
	{
		CollateExpr *c = (CollateExpr *) node;

		return strip_collate_mutator((Node *) c->arg);
	}
	if (IsA(node, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) node;
		OpExpr	   *newop = makeNode(OpExpr);
		ListCell   *lc;

		*newop = *op;
		newop->args = NIL;
		foreach(lc, op->args)
			newop->args = lappend(newop->args,
								  strip_collate_mutator((Node *) lfirst(lc)));
		return (Node *) newop;
	}
	if (IsA(node, FuncExpr))
	{
		FuncExpr   *f = (FuncExpr *) node;
		FuncExpr   *newf = makeNode(FuncExpr);
		ListCell   *lc;

		*newf = *f;
		newf->args = NIL;
		foreach(lc, f->args)
			newf->args = lappend(newf->args,
								 strip_collate_mutator((Node *) lfirst(lc)));
		return (Node *) newf;
	}
	if (IsA(node, RelabelType))
	{
		RelabelType *r = (RelabelType *) node;
		RelabelType *newr = makeNode(RelabelType);

		*newr = *r;
		newr->arg = (Expr *)
			strip_collate_mutator((Node *) r->arg);
		return (Node *) newr;
	}
	if (IsA(node, CoerceViaIO))
	{
		CoerceViaIO *c = (CoerceViaIO *) node;
		CoerceViaIO *newc = makeNode(CoerceViaIO);

		*newc = *c;
		newc->arg = (Expr *)
			strip_collate_mutator((Node *) c->arg);
		return (Node *) newc;
	}
	if (IsA(node, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) node;
		BoolExpr   *newb = makeNode(BoolExpr);
		ListCell   *lc;

		*newb = *b;
		newb->args = NIL;
		foreach(lc, b->args)
			newb->args = lappend(newb->args,
								 strip_collate_mutator((Node *) lfirst(lc)));
		return (Node *) newb;
	}
	if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *s = (ScalarArrayOpExpr *) node;
		ScalarArrayOpExpr *news = makeNode(ScalarArrayOpExpr);
		ListCell   *lc;

		*news = *s;
		news->args = NIL;
		foreach(lc, s->args)
			news->args = lappend(news->args,
								 strip_collate_mutator((Node *) lfirst(lc)));
		return (Node *) news;
	}
	if (IsA(node, NullTest))
	{
		NullTest   *n = (NullTest *) node;
		NullTest   *newn = makeNode(NullTest);

		*newn = *n;
		newn->arg = (Expr *)
			strip_collate_mutator((Node *) n->arg);
		return (Node *) newn;
	}
	if (IsA(node, BooleanTest))
	{
		BooleanTest *b = (BooleanTest *) node;
		BooleanTest *newb = makeNode(BooleanTest);

		*newb = *b;
		newb->arg = (Expr *)
			strip_collate_mutator((Node *) b->arg);
		return (Node *) newb;
	}
	/* For Const, Param, Var, etc. -- return a copy */
	return copyObject(node);
}

/*
 * resolve_graph_property
 *
 *		At runtime, look up the property expression for a computed
 *		property that is not a real column in the vertex table.
 *
 *		The property expression is stored in pg_propgraph_label_property
 *		and was set during CREATE PROPERTY GRAPH.  For a constant
 *		property like "'order'::varchar(10) AS list_type", the expression
 *		is a Const node.
 *
 *		We find the matching element OID by looking at which physical
 *		table the elem_slot's tuple descriptor corresponds to, then
 *		look up the label-property binding expression for that element
 *		and property.
 */
static Datum
resolve_graph_property(GraphScanState * node, int index,
					   Oid propid, TupleTableSlot *slot,
					   bool *isnull, bool is_edge)
{
	/* For property graphs */
	Relation	pl_rel;
	SysScanDesc pl_scan;
	HeapTuple	pl_tup;

	/* For the elements */
	Relation	el_rel;
	SysScanDesc el_scan;
	HeapTuple	el_tup;
	char		element_kind;

	*isnull = true;

	if (slot == NULL || slot->tts_tupleDescriptor == NULL)
		return (Datum) 0;

	element_kind = is_edge ? 'e' : 'v';

	/* Find which element OID this slot corresponds to */
	for (int ei = 0; ei < node->num_elements; ei++)
	{
		if (node->element_info[ei].element_kind != element_kind)
			continue;
		if (node->element_info[ei].heap_rel &&
			node->element_info[ei].tupdesc->tdtypeid ==
			slot->tts_tupleDescriptor->tdtypeid)
		{
			Oid			elemoid = node->element_info[ei].element_oid;

			/* Look up the property expression in the catalog */
			pl_rel = table_open(PropgraphLabelPropertyRelationId,
								AccessShareLock);
			pl_scan = systable_beginscan(pl_rel, InvalidOid, false,
										 NULL, 0, NULL);
			while (HeapTupleIsValid(pl_tup = systable_getnext(pl_scan)))
			{
				Form_pg_propgraph_label_property pl_form =
					(Form_pg_propgraph_label_property) GETSTRUCT(pl_tup);

				if (pl_form->plppropid != propid)
					continue;

				/*
				 * Check if this label-property binding's element matches our
				 * vertex
				 */
				el_rel = table_open(
									PropgraphElementLabelRelationId,
									AccessShareLock);
				el_scan = systable_beginscan(el_rel,
											 InvalidOid, false,
											 NULL, 0, NULL);
				while (HeapTupleIsValid(el_tup = systable_getnext(el_scan)))
				{
					Form_pg_propgraph_element_label el_form =
						(Form_pg_propgraph_element_label) GETSTRUCT(el_tup);

					if (el_form->pgelelid == elemoid && el_form->oid == pl_form->plpellabelid)
					{
						Datum		exprdatum;
						bool		exprnull;
						Node	   *exprnode;
						ExprState  *exprstate;
						ExprContext *econtext;
						Datum		result;

						exprdatum = heap_getattr(pl_tup,
												 Anum_pg_propgraph_label_property_plpexpr,
												 RelationGetDescr(pl_rel),
												 &exprnull);
						if (exprnull)
						{
							result = (Datum) 0;
						}
						else
						{
							exprnode = stringToNode(TextDatumGetCString(exprdatum));

							/* Strip CollateExpr wrappers */
							exprnode = strip_collate_mutator(exprnode);

							/* Compile and evaluate */
							exprstate = ExecInitExpr((Expr *) exprnode, (PlanState *) NULL);
							econtext = node->ss.ps.ps_ExprContext;
							econtext->ecxt_scantuple = slot;
							ResetExprContext(econtext);
							result = ExecEvalExpr(exprstate, econtext, isnull);
							pfree(exprnode);
						}
						systable_endscan(el_scan);
						table_close(el_rel, AccessShareLock);
						systable_endscan(pl_scan);
						table_close(pl_rel, AccessShareLock);
						return result;
					}
				}
				systable_endscan(el_scan);
				table_close(el_rel, AccessShareLock);
			}
			systable_endscan(pl_scan);
			table_close(pl_rel, AccessShareLock);
			return (Datum) 0;
		}
	}

	return (Datum) 0;
}

/*
 * Walk an expression tree to find a Var referencing the given varno
 * and return its varattno.  Returns -1 if not found.
 */
static int
find_varattno_in_expr(Node *node, Index varno)
{
	if (node == NULL)
		return -1;

	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == varno)
			return var->varattno;
		return -1;
	}

	if (IsA(node, FuncExpr))
	{
		FuncExpr   *f = (FuncExpr *) node;
		ListCell   *lc;

		foreach(lc, f->args)
		{
			int			attno = find_varattno_in_expr((Node *) lfirst(lc), varno);

			if (attno > 0)
				return attno;
		}
		return -1;
	}

	if (IsA(node, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) node;
		ListCell   *lc;

		foreach(lc, op->args)
		{
			int			attno = find_varattno_in_expr((Node *) lfirst(lc), varno);

			if (attno > 0)
				return attno;
		}
		return -1;
	}

	if (IsA(node, RelabelType))
		return find_varattno_in_expr((Node *) ((RelabelType *) node)->arg, varno);

	if (IsA(node, CoerceViaIO))
		return find_varattno_in_expr((Node *) ((CoerceViaIO *) node)->arg, varno);

	/* For BoolExpr, FuncExpr, etc.: scan args */
	if (IsA(node, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) node;
		ListCell   *lc;

		foreach(lc, b->args)
		{
			int			attno = find_varattno_in_expr((Node *) lfirst(lc), varno);

			if (attno > 0)
				return attno;
		}
		return -1;
	}

	return -1;
}

/*
 * Walk an expression tree to find a GraphPropertyRef and return the
 * pattern element index it references.  Returns -1 if not found.
 */
static int
find_ref_elem_index(Node *node, GraphScanState * scanstate)
{
	int			i;

	if (node == NULL)
		return -1;

	if (IsA(node, GraphPropertyRef))
	{
		GraphPropertyRef *gpr = (GraphPropertyRef *) node;
		const char *varname = gpr->elvarname;
		List	   *path_elements;

		path_elements = linitial(((GraphScan *) scanstate->ss.ps.plan)->graph_pattern->path_pattern_list);

		for (i = 0; i < list_length(path_elements); i++)
		{
			GraphElementPattern *gep = (GraphElementPattern *) list_nth(path_elements, i);

			if (gep->variable && strcmp(gep->variable, varname) == 0)
				return i;
		}
		return -1;
	}

	if (IsA(node, FuncExpr))
	{
		FuncExpr   *f = (FuncExpr *) node;
		ListCell   *lc;

		foreach(lc, f->args)
		{
			int			idx = find_ref_elem_index((Node *) lfirst(lc), scanstate);

			if (idx >= 0)
				return idx;
		}
	}

	if (IsA(node, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) node;
		ListCell   *lc;

		foreach(lc, op->args)
		{
			int			idx = find_ref_elem_index((Node *) lfirst(lc), scanstate);

			if (idx >= 0)
				return idx;
		}
	}

	if (IsA(node, RelabelType))
		return find_ref_elem_index((Node *) ((RelabelType *) node)->arg, scanstate);
	if (IsA(node, CoerceViaIO))
		return find_ref_elem_index((Node *) ((CoerceViaIO *) node)->arg, scanstate);
	if (IsA(node, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) node;
		ListCell   *lc;

		foreach(lc, b->args)
		{
			int			idx = find_ref_elem_index((Node *) lfirst(lc), scanstate);

			if (idx >= 0)
				return idx;
		}
	}

	return -1;
}

/*
 * Convert a GraphPropertyRef node to a Var(OUTER_VAR, attnum) with the
 * appropriate attribute number for reading the property from the element
 * tuple slot.  Returns a new copy of the expression tree.
 */
static Node *
convert_GraphPropertyRef_to_Var_internal(Node *node, int elem_idx,
										 GraphScanState * scanstate)
{
	if (node == NULL)
		return NULL;

	if (IsA(node, GraphPropertyRef))
	{
		GraphPropertyRef *gpr = (GraphPropertyRef *) node;
		char	   *propname;
		AttrNumber	attnum = InvalidAttrNumber;
		int			ei;

		propname = get_propgraph_property_name(gpr->propid);

		/*
		 * Find the physical table attribute by scanning element_info for a
		 * tuple descriptor that has this property name.
		 */
		for (ei = 0; ei < scanstate->num_elements; ei++)
		{
			TupleDesc	tupdesc = scanstate->element_info[ei].tupdesc;
			int			ai;

			for (ai = 0; ai < tupdesc->natts; ai++)
			{
				Form_pg_attribute attr = TupleDescAttr(tupdesc, ai);

				if (strcmp(NameStr(attr->attname), propname) == 0)
				{
					attnum = ai + 1;
					break;
				}
			}
			if (attnum != InvalidAttrNumber)
				break;
		}

		if (attnum == InvalidAttrNumber)
		{
			/* Property not found in any backing table, fallback to 1 */
			attnum = 1;
		}

		return (Node *) makeVar(OUTER_VAR, attnum,
								gpr->typeId, gpr->typmod,
								gpr->collation, 0);
	}

	if (IsA(node, FuncExpr))
	{
		FuncExpr   *f = (FuncExpr *) node;
		FuncExpr   *newf = makeNode(FuncExpr);
		ListCell   *lc;

		*newf = *f;
		newf->args = NIL;
		foreach(lc, f->args)
			newf->args = lappend(newf->args,
								 convert_GraphPropertyRef_to_Var_internal(
																		  (Node *) lfirst(lc), elem_idx, scanstate));
		return (Node *) newf;
	}

	if (IsA(node, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) node;
		OpExpr	   *newop = makeNode(OpExpr);
		ListCell   *lc;

		*newop = *op;
		newop->args = NIL;
		foreach(lc, op->args)
			newop->args = lappend(newop->args,
								  convert_GraphPropertyRef_to_Var_internal(
																		   (Node *) lfirst(lc), elem_idx, scanstate));
		return (Node *) newop;
	}

	if (IsA(node, RelabelType))
	{
		RelabelType *r = (RelabelType *) node;
		RelabelType *newr = makeNode(RelabelType);

		*newr = *r;
		newr->arg = (Expr *)
			convert_GraphPropertyRef_to_Var_internal(
													 (Node *) r->arg, elem_idx, scanstate);
		return (Node *) newr;
	}

	if (IsA(node, CoerceViaIO))
	{
		CoerceViaIO *c = (CoerceViaIO *) node;
		CoerceViaIO *newc = makeNode(CoerceViaIO);

		*newc = *c;
		newc->arg = (Expr *)
			convert_GraphPropertyRef_to_Var_internal(
													 (Node *) c->arg, elem_idx, scanstate);
		return (Node *) newc;
	}

	if (IsA(node, CollateExpr))
	{
		CollateExpr *c = (CollateExpr *) node;
		CollateExpr *newc = makeNode(CollateExpr);

		*newc = *c;
		newc->arg = (Expr *)
			convert_GraphPropertyRef_to_Var_internal(
													 (Node *) c->arg, elem_idx, scanstate);
		return (Node *) newc;
	}

	if (IsA(node, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) node;
		BoolExpr   *newb = makeNode(BoolExpr);
		ListCell   *lc;

		*newb = *b;
		newb->args = NIL;
		foreach(lc, b->args)
			newb->args = lappend(newb->args,
								 convert_GraphPropertyRef_to_Var_internal(
																		  (Node *) lfirst(lc), elem_idx, scanstate));
		return (Node *) newb;
	}

	/* For other node types, return a copy unchanged */
	return copyObject(node);
}

static void
convert_GraphPropertyRef_to_Var(Node **node_p, int elem_idx,
								GraphScanState * scanstate)
{
	Node	   *result = convert_GraphPropertyRef_to_Var_internal(
																  *node_p, elem_idx, scanstate);

	if (result)
		*node_p = result;
}

/*
 * Check whether the given property (identified by propid) exists on ALL
 * candidate element tables for pattern element pi.  Returns true if the
 * property is present on every candidate, false if at least one candidate
 * lacks it.  Used to determine whether a direct slot read is safe, or
 * whether a label-aware runtime check is needed.
 */
static bool
is_property_on_all_candidates(GraphScanState * node, int pi, Oid propid)
{
	GraphScan  *plan = (GraphScan *) node->ss.ps.plan;
	List	   *path = linitial(plan->graph_pattern->path_pattern_list);
	GraphElementPattern *gep = (GraphElementPattern *) list_nth(path, pi);
	Relation	pg_elem;
	SysScanDesc elem_scan;
	HeapTuple	elem_tup;
	List	   *candidates = NIL;
	ListCell   *lc;

	if (gep == NULL)
		return true;

	pg_elem = table_open(PropgraphElementRelationId, AccessShareLock);
	elem_scan = systable_beginscan(pg_elem, InvalidOid, false, NULL, 0, NULL);

	while (HeapTupleIsValid(elem_tup = systable_getnext(elem_scan)))
	{
		Form_pg_propgraph_element elform =
			(Form_pg_propgraph_element) GETSTRUCT(elem_tup);

		if (elform->pgepgid != plan->graph_oid)
			continue;
		if (IS_EDGE_PATTERN(gep->kind))
		{
			if (elform->pgekind != 'e')
				continue;
		}
		else
		{
			if (elform->pgekind != 'v')
				continue;
		}
		candidates = lappend_oid(candidates, elform->oid);
	}
	systable_endscan(elem_scan);
	table_close(pg_elem, AccessShareLock);

	/* Filter by label expression */
	if (gep->labelexpr != NULL)
	{
		List	   *filtered = NIL;

		foreach(lc, candidates)
		{
			if (element_matches_label(lfirst_oid(lc), gep->labelexpr))
				filtered = lappend_oid(filtered, lfirst_oid(lc));
		}
		candidates = filtered;
	}

	if (candidates == NIL)
		return true;

	foreach(lc, candidates)
	{
		if (!check_property_on_element(lfirst_oid(lc), propid,
									   gep->labelexpr))
			return false;
	}
	return true;
}

/*
 * Build the column mapping arrays from the plan target list and
 * graph_table_columns.  Each entry maps a plan target list position
 * to the pattern element and table attribute that provides its value.
 */
static void
init_column_mapping(GraphScanState * node, EState *estate)
{
	GraphScan  *plan = (GraphScan *) node->ss.ps.plan;
	RangeTblEntry *rte;
	List	   *gtcols;
	int			rti = plan->scan.scanrelid;
	int			gtcol_count;
	int			ncols;
	int			ci;
	TargetEntry *gte;
	int			varattno;

	rte = exec_rt_fetch(rti, estate);
	gtcols = rte->graph_table_columns;
	gtcol_count = list_length(gtcols);
	ncols = list_length(plan->scan.plan.targetlist);

	node->ncols = ncols;
	node->col_elem_index = (int *) palloc0(sizeof(int) * ncols);
	node->col_is_const = (bool *) palloc0(sizeof(bool) * ncols);
	node->col_const_value = (Datum *) palloc0(sizeof(Datum) * ncols);
	node->col_const_null = (bool *) palloc0(sizeof(bool) * ncols);
	node->col_propname = (char **) palloc0(sizeof(char *) * ncols);
	node->col_propid = (Oid *) palloc0(sizeof(Oid) * ncols);
	node->col_prop_unsafe = (bool *) palloc0(sizeof(bool) * ncols);

	for (ci = 0; ci < ncols; ci++)
	{
		TargetEntry *te = (TargetEntry *)
			list_nth(plan->scan.plan.targetlist, ci);

		node->col_elem_index[ci] = -1;
		node->col_is_const[ci] = false;
		node->col_const_value[ci] = (Datum) 0;
		node->col_const_null[ci] = true;
		node->col_propid[ci] = InvalidOid;

		/*
		 * Determine which graph table column index this target list entry
		 * corresponds to.
		 */
		if (IsA(te->expr, Var) &&
			((Var *) te->expr)->varno == rti)
		{
			varattno = ((Var *) te->expr)->varattno;
		}
		else
		{
			/*
			 * Non-Var target list entries (e.g. FuncExpr, RelabelType) still
			 * reference a graph table column position. Walk the expression
			 * tree to find Var references.
			 */
			varattno = find_varattno_in_expr((Node *) te->expr, rti);
		}

		if (varattno <= 0 || varattno > gtcol_count)
			continue;

		gte = (TargetEntry *) list_nth(gtcols, varattno - 1);

		{
			Node	   *gcol = (Node *) gte->expr;

			/* Unwrap wrapper expressions (FuncExpr, RelabelType) */
			while (IsA(gcol, FuncExpr) || IsA(gcol, RelabelType) ||
				   IsA(gcol, CoerceViaIO))
			{
				if (IsA(gcol, FuncExpr))
					gcol = (Node *) linitial(((FuncExpr *) gcol)->args);
				else if (IsA(gcol, RelabelType))
					gcol = (Node *) ((RelabelType *) gcol)->arg;
				else if (IsA(gcol, CoerceViaIO))
					gcol = (Node *) ((CoerceViaIO *) gcol)->arg;
			}

			if (IsA(gcol, Const))
			{
				Const	   *c = (Const *) gcol;

				node->col_is_const[ci] = true;
				node->col_const_value[ci] = c->constvalue;
				node->col_const_null[ci] = c->constisnull;
			}
			else if (IsA(gcol, GraphPropertyRef))
			{
				GraphPropertyRef *gpr = (GraphPropertyRef *) gcol;
				const char *varname = gpr->elvarname;
				char	   *propname;
				int			pi;
				List	   *path_elements;

				propname = get_propgraph_property_name(gpr->propid);
				node->col_propname[ci] = pstrdup(propname);
				node->col_propid[ci] = gpr->propid;
				path_elements = linitial(plan->graph_pattern->path_pattern_list);

				for (pi = 0; pi < list_length(path_elements); pi++)
				{
					GraphElementPattern *gep = (GraphElementPattern *)
						list_nth(path_elements, pi);

					if (!gep->variable ||
						strcmp(gep->variable, varname) != 0)
						continue;

					node->col_elem_index[ci] = pi;
					break;
				}

				/*
				 * Mark as unsafe if the property does not exist on ALL
				 * candidate label tables.  At runtime, we will use
				 * resolve_graph_property (which checks label-property
				 * bindings) instead of a direct physical read.
				 */
				if (node->col_elem_index[ci] >= 0 &&
					!is_property_on_all_candidates(node,
												   node->col_elem_index[ci],
												   node->col_propid[ci]))
					node->col_prop_unsafe[ci] = true;
			}
		}
	}
}

/* ----------------------------------------------------------------
 *	 init_start_vertex
 *
 *		Begin scanning the start vertex table.  Starts with the
 *		first matching vertex table at table_index=0.
 * ----------------------------------------------------------------
 */
static void
init_start_vertex(GraphScanState * node)
{
	GraphElementPattern *elem = GetPatternElement(node, 0);
	GraphElementRelInfo *rel_info;

	node->start_table_index = 0;
	node->current_hop = -1;

	if (elem == NULL)
		return;

	rel_info = GetElementRelInfo(node, elem, 0);

	/* Start a sequential scan on the first matching vertex table */
	if (rel_info)
	{
		check_table_privilege(rel_info->table_oid);
		node->start_scan = table_beginscan(rel_info->heap_rel,
										   node->ss.ps.state->es_snapshot,
										   0, NULL, SO_NONE);
		node->start_vertex_slot =
			MakeSingleTupleTableSlot(rel_info->tupdesc,
									 table_slot_callbacks(rel_info->heap_rel));
	}
}

/* ----------------------------------------------------------------
 *	 start_vertex_advance
 *
 *		Advance to the next start vertex.  Handles multi-table
 *		iteration: when one vertex table is exhausted, tries the
 *		next matching vertex table.  Returns true if a new start
 *		vertex was found, false if all tables are exhausted.
 * ----------------------------------------------------------------
 */
static bool
start_vertex_advance(GraphScanState * node)
{
	GraphElementPattern *elem = GetPatternElement(node, 0);
	GraphElementRelInfo *rel_info;

	if (elem == NULL)
		return false;

	for (;;)
	{
		rel_info = GetElementRelInfo(node, elem, node->start_table_index);
		if (rel_info == NULL)
		{
			/* No more matching vertex tables */
			return false;
		}

		/* If we have a scan already, try the next tuple */
		if (node->start_scan != NULL)
		{
			if (table_scan_getnextslot(node->start_scan,
									   ForwardScanDirection,
									   node->start_vertex_slot))
			{
				/* extract PK values and check WHERE clause for vertex */
				node->n_start_keys = rel_info->nkey_attnums;
				if (node->start_vertex_values)
					pfree(node->start_vertex_values);
				if (node->start_vertex_nulls)
					pfree(node->start_vertex_nulls);
				node->start_vertex_values = (Datum *)
					palloc(sizeof(Datum) * rel_info->nkey_attnums);
				node->start_vertex_nulls = (bool *)
					palloc(sizeof(bool) * rel_info->nkey_attnums);
				for (int ki = 0; ki < rel_info->nkey_attnums; ki++)
					node->start_vertex_values[ki] =
						slot_getattr(node->start_vertex_slot,
									 rel_info->key_attnums[ki],
									 &node->start_vertex_nulls[ki]);

				/* Check WHERE clause on element 0 (start vertex) */
				if (node->element_quals[0] != NULL)
				{
					if (!evaluate_graph_where_clause(
													 (Node *) node->element_quals[0],
													 node->start_vertex_slot,
													 node->ss.ps.ps_ExprContext,
													 node, 0))
						continue;
				}

				/* Materialize so the slot survives across scan advances */
				ExecMaterializeSlot(node->start_vertex_slot);

				/* Store PK identity for same-var tracking if pi=0 is shared */
				store_start_vertex_identity(node);

				return true;
			}

			/* Exhausted this table, close scan and move to next table */
			table_endscan(node->start_scan);
			node->start_scan = NULL;
			ExecDropSingleTupleTableSlot(node->start_vertex_slot);
			node->start_vertex_slot = NULL;
		}

		/* Advance to next matching vertex table */
		node->start_table_index++;
		rel_info = GetElementRelInfo(node, elem, node->start_table_index);
		if (rel_info == NULL)
			return false;

		/* Start scanning the next table */
		check_table_privilege(rel_info->table_oid);
		node->start_scan = table_beginscan(rel_info->heap_rel,
										   node->ss.ps.state->es_snapshot,
										   0, NULL, SO_NONE);
		node->start_vertex_slot =
			MakeSingleTupleTableSlot(rel_info->tupdesc,
									 table_slot_callbacks(rel_info->heap_rel));
	}
}

/* ----------------------------------------------------------------
 *	 find_edge_table_for_vertex
 *
 *		Find the edge_table_index-th matching edge catalog element
 *		for a given vertex element OID, filtered by label expression
 *		and traversal direction.
 *
 *		For OUTGOING or undirected pass 0: matches edges where
 *		src_vertex_elemid == elemoid (the vertex is the source).
 *		For INCOMING or undirected pass 1: matches edges where
 *		dest_vertex_elemid == elemoid (the vertex is the destination).
 *
 *		Returns the catalog element index, or -1 if no match.
 * ----------------------------------------------------------------
 */
static int
find_edge_table_for_vertex(GraphScanState * node, Oid elemoid,
						   Node *labelexpr, int table_index,
						   EdgeDirection dir, int scan_pass)
{
	int			count = 0;
	int			i;
	bool		is_incoming = edge_is_incoming(dir, scan_pass);

	for (i = 0; i < node->num_elements; i++)
	{
		if (node->element_info[i].element_kind != 'e')
			continue;

		if (is_incoming)
		{
			if (node->element_info[i].dest_vertex_elemid != elemoid)
				continue;
		}
		else
		{
			if (node->element_info[i].src_vertex_elemid != elemoid)
				continue;
		}

		if (labelexpr != NULL &&
			!element_matches_label_expr(node, i, labelexpr))
			continue;

		if (count == table_index)
			return i;
		count++;
	}
	return -1;
}

/* ----------------------------------------------------------------
 *	 vle_find_next_edge
 *
 *		Find the next edge from the given source vertex, scanning
 *		edge tables and handling undirected passes.  Unlike
 *		hop_scan_edges, the source vertex identity is passed as
 *		parameters (src_elemid, src_values, etc.) rather than read
 *		from hop->from_vertex_*.  On entry, *start_table_idx and
 *		*start_scan_pass indicate where to resume scanning; on
 *		return (found) they are updated to the resume position.
 *
 *		On success, sets *dest_elemid, *dest_nkeys, *dest_values,
 *		*dest_nulls with the matched destination vertex, and returns
 *		true.  On exhaustion, returns false.
 *
 *		Edge scan state is stored on the GraphHopFrame (edge_scan,
 *		edge_slot), shared across all stack frame levels.
 * ----------------------------------------------------------------
 */
static bool
vle_find_next_edge(GraphScanState * node,
				   Oid src_elemid,
				   int src_nkeys, Datum *src_values,
				   bool *src_nulls,
				   int *start_table_idx, int *start_scan_pass,
				   int edge_elem_idx, EdgeDirection dir,
				   GraphHopFrame * hop,
				   Oid *dest_elemid,
				   int *dest_nkeys, Datum **dest_values,
				   bool **dest_nulls)
{
	GraphElementPattern *edge_elem = GetPatternElement(node, edge_elem_idx);
	GraphElementRelInfo *edge_info;

	if (src_elemid == InvalidOid)
		return false;

	for (;;)
	{
		int			edge_cat_idx;

		/* Find which edge catalog table to scan */
		edge_cat_idx = find_edge_table_for_vertex(node, src_elemid,
												  edge_elem ? edge_elem->labelexpr : NULL,
												  *start_table_idx,
												  dir, *start_scan_pass);

		if (edge_cat_idx < 0)
		{
			/* No more matching edge tables. Try undirected second pass */
			if (dir == GRAPH_DIR_UNDIRECTED && *start_scan_pass == 0)
			{
				*start_scan_pass = 1;
				*start_table_idx = 0;
				hop_close_edge_scan(hop);
				continue;
			}
			return false;
		}

		edge_info = &node->element_info[edge_cat_idx];

		/* Start a heap scan if not already started for this table */
		if (hop->edge_scan == NULL ||
			!hop->edge_slot ||
			hop->edge_slot->tts_tupleDescriptor != edge_info->tupdesc)
		{
			hop_close_edge_scan(hop);

			check_table_privilege(edge_info->table_oid);
			hop->edge_scan = table_beginscan(edge_info->heap_rel,
											 node->ss.ps.state->es_snapshot,
											 0, NULL, SO_NONE);
			hop->edge_slot =
				MakeSingleTupleTableSlot(edge_info->tupdesc,
										 table_slot_callbacks(edge_info->heap_rel));
		}

		/* Scan through edge tuples */
		while (table_scan_getnextslot(hop->edge_scan, ForwardScanDirection,
									  hop->edge_slot))
		{
			bool		match = false;
			int			fk_attr_count;

			/*
			 * On pass 1 of an undirected scan, skip edges from the same table
			 * whose TID was already accepted on pass 0.
			 */
			if (dir == GRAPH_DIR_UNDIRECTED && *start_scan_pass == 1 &&
				hop->undirected_has_seen &&
				hop->undirected_seen_edge_idx == edge_cat_idx &&
				ItemPointerEquals(&hop->edge_slot->tts_tid,
								  &hop->undirected_seen_tid))
				continue;

			/* Determine which FK columns to check based on direction */
			if (edge_is_incoming(dir, *start_scan_pass))
				fk_attr_count = edge_info->ndst_fk_attnums;
			else
				fk_attr_count = edge_info->nsrc_fk_attnums;

			if (edge_info->nsrc_fk_attnums > 0)
			{
				int			ki;

				match = true;
				for (ki = 0; ki < fk_attr_count; ki++)
				{
					AttrNumber	fk_attnum;
					Datum		edge_val;
					bool		fk_isnull;

					if (edge_is_incoming(dir, *start_scan_pass))
						fk_attnum = edge_info->dst_fk_attnums[ki];
					else
						fk_attnum = edge_info->src_fk_attnums[ki];

					edge_val = slot_getattr(hop->edge_slot,
											fk_attnum, &fk_isnull);

					if (fk_isnull || ki >= src_nkeys ||
						src_nulls[ki] ||
						DatumGetInt32(edge_val) !=
						DatumGetInt32(src_values[ki]))
					{
						match = false;
						break;
					}
				}
			}
			else
				match = true;

			if (!match)
				continue;

			/* Check WHERE clause on this edge element */
			if (edge_elem_idx >= 0 &&
				edge_elem_idx < node->n_pattern_elems &&
				node->element_quals[edge_elem_idx] != NULL)
			{
				if (!evaluate_graph_where_clause(
												 (Node *) node->element_quals[edge_elem_idx],
												 hop->edge_slot, node->ss.ps.ps_ExprContext,
												 node, edge_elem_idx))
					continue;
			}

			/* Check same-variable identity for edges */
			if (!check_samevar_edge_identity(node, edge_elem_idx, hop))
				continue;

			/* Found a matching edge. Materialize and fetch dest vertex */
			ExecMaterializeSlot(hop->edge_slot);

			/* Extract target vertex keys */
			{
				int			n_fk_cols;
				int			dest_vert_idx;

				if (edge_is_incoming(dir, *start_scan_pass))
				{
					n_fk_cols = edge_info->nsrc_fk_attnums;
					dest_vert_idx = find_vertex_for_edge_src(node, edge_cat_idx);
				}
				else
				{
					n_fk_cols = edge_info->ndst_fk_attnums;
					dest_vert_idx = find_vertex_for_edge_dest(node, edge_cat_idx);
				}

				if (*dest_values)
					pfree(*dest_values);
				if (*dest_nulls)
					pfree(*dest_nulls);
				*dest_nkeys = n_fk_cols;
				*dest_values = (Datum *) palloc(sizeof(Datum) * n_fk_cols);
				*dest_nulls = (bool *) palloc(sizeof(bool) * n_fk_cols);
				ExtractTargetVertexKeys(hop->edge_slot, edge_info,
										dir, *start_scan_pass,
										n_fk_cols,
										*dest_values,
										*dest_nulls);

				if (dest_vert_idx >= 0)
				{
					GraphElementRelInfo *dest_info =
						&node->element_info[dest_vert_idx];
					TupleTableSlot *dest_slot;

					/* Fetch dest vertex via PK lookup */
					dest_slot = fetch_vertex_by_pk(dest_info,
												   *dest_values,
												   *dest_nulls,
												   n_fk_cols,
												   node->ss.ps.state);
					if (dest_slot == NULL)
						continue;

					/*
					 * Validate that destination vertex matches the label
					 * expression of the next pattern element.
					 */
					{
						int			next_elem_idx = edge_elem_idx + 1;

						if (next_elem_idx < node->n_pattern_elems)
						{
							GraphElementPattern *next_elem =
								GetPatternElement(node, next_elem_idx);

							if (next_elem && next_elem->labelexpr &&
								!element_matches_label_expr(
															node, dest_vert_idx,
															next_elem->labelexpr))
							{
								ExecDropSingleTupleTableSlot(dest_slot);
								continue;
							}
						}
					}

					/* Check WHERE clause on destination vertex element */
					{
						int			dest_elem_idx = edge_elem_idx + 1;

						if (dest_elem_idx < node->n_pattern_elems &&
							node->element_quals[dest_elem_idx] != NULL)
						{
							if (!evaluate_graph_where_clause(
															 (Node *) node->element_quals[dest_elem_idx],
															 dest_slot,
															 node->ss.ps.ps_ExprContext,
															 node, dest_elem_idx))
							{
								ExecDropSingleTupleTableSlot(dest_slot);
								continue;
							}
						}
					}

					/* Check same-variable vertex identity */
					{
						int			dest_elem_idx = edge_elem_idx + 1;

						if (!check_samevar_vertex_identity(
														   node, dest_elem_idx, hop))
						{
							ExecDropSingleTupleTableSlot(dest_slot);
							continue;
						}
					}

					/* Store the dest vertex slot (discard old one) */
					if (hop->to_vertex_slot)
						ExecDropSingleTupleTableSlot(hop->to_vertex_slot);
					hop->to_vertex_slot = dest_slot;
					{
						/*
						 * For incoming traversal, the "to" vertex is the
						 * edge's source element.
						 */
						*dest_elemid =
							edge_is_incoming(dir, *start_scan_pass) ?
							node->element_info[edge_cat_idx].src_vertex_elemid :
							node->element_info[edge_cat_idx].dest_vertex_elemid;
					}

					/*
					 * For undirected scans: store the TID of an edge accepted
					 * on pass 0.
					 */
					if (dir == GRAPH_DIR_UNDIRECTED &&
						*start_scan_pass == 0 && hop->edge_slot)
					{
						if (node->element_info[edge_cat_idx].src_vertex_elemid ==
							node->element_info[edge_cat_idx].dest_vertex_elemid)
						{
							ItemPointerCopy(&hop->edge_slot->tts_tid,
											&hop->undirected_seen_tid);
							hop->undirected_seen_edge_idx = edge_cat_idx;
							hop->undirected_has_seen = true;
						}
					}

					return true;
				}
			}
		}

		/* No more tuples in this edge table, try next table */
		(*start_table_idx)++;
		hop_close_edge_scan(hop);
	}
}

/* ----------------------------------------------------------------
 *	 hop_scan_edges
 *
 *		Find the next edge from the current hop's source vertex.
 *		Handles multi-edge-table iteration and undirected passes.
 *		Returns true if a matching edge+dest vertex was found,
 *		false if all edges for this source are exhausted.
 *
 *		Thin wrapper around vle_find_next_edge using the hop's
 *		from_vertex_* as the source.
 * ----------------------------------------------------------------
 */
static bool
hop_scan_edges(GraphScanState * node, GraphHopFrame * hop,
			   int edge_elem_idx, EdgeDirection dir)
{
	return vle_find_next_edge(node,
							  hop->from_vertex_elemid,
							  hop->nfrom, hop->from_vertex_values,
							  hop->from_vertex_nulls,
							  &hop->edge_table_index, &hop->scan_pass,
							  edge_elem_idx, dir,
							  hop,
							  &hop->to_vertex_elemid,
							  &hop->nto, &hop->to_vertex_values,
							  &hop->to_vertex_nulls);
}

/*
 * For a same-var group whose first occurrence is pi=0, store the start
 * vertex's PK values when we fetch a new start vertex.  Called from
 * start_vertex_advance after a start vertex is accepted.
 */
static void
store_start_vertex_identity(GraphScanState * node)
{
	int			pi = 0;
	int			gid;

	if (pi >= node->num_pi)
		return;
	gid = node->samevar_group[pi];
	if (gid < 0)
		return;
	if (node->samevar_elem_kind[gid] != 'v')
		return;

	/* Store PK values from the start vertex */
	node->samevar_nkeys[gid] = node->n_start_keys;
	if (node->n_start_keys > 0)
	{
		int			ki;

		if (node->samevar_key_values[gid])
			pfree(node->samevar_key_values[gid]);
		if (node->samevar_key_nulls[gid])
			pfree(node->samevar_key_nulls[gid]);

		node->samevar_key_values[gid] = (Datum *)
			palloc(sizeof(Datum) * node->n_start_keys);
		node->samevar_key_nulls[gid] = (bool *)
			palloc(sizeof(bool) * node->n_start_keys);

		for (ki = 0; ki < node->n_start_keys; ki++)
		{
			node->samevar_key_values[gid][ki] =
				node->start_vertex_values[ki];
			node->samevar_key_nulls[gid][ki] =
				node->start_vertex_nulls[ki];
		}
	}
}

/*
 * Check whether the edge at edge_elem_idx must match a previously-occurring
 * edge with the same variable name.  Returns false if the edge should be
 * rejected (identity mismatch).
 */
static bool
check_samevar_edge_identity(GraphScanState * node, int edge_elem_idx,
							GraphHopFrame * hop)
{
	int			gid;
	int			first_pi;

	if (edge_elem_idx < 0 || edge_elem_idx >= node->num_pi)
		return true;
	gid = node->samevar_group[edge_elem_idx];
	if (gid < 0)
		return true;
	if (node->samevar_elem_kind[gid] != 'e')
		return true;

	first_pi = node->samevar_first_pi[gid];

	if (edge_elem_idx == first_pi)
	{
		/* First occurrence: store TID for future comparisons */
		if (hop->edge_slot)
			ItemPointerCopy(&hop->edge_slot->tts_tid,
							&node->samevar_tid[gid]);
		return true;
	}
	else
	{
		/* Compare TID */
		if (!hop->edge_slot)
			return false;
		return ItemPointerEquals(&hop->edge_slot->tts_tid,
								 &node->samevar_tid[gid]);
	}
}

/*
 * Check whether the destination vertex at dest_elem_idx must match a
 * previously-occurring vertex with the same variable name.  Returns false
 * if the vertex should be rejected (identity mismatch).
 *
 * If this is the first occurrence (not pi=0, handled separately), stores
 * the PK values for future comparisons.
 */
static bool
check_samevar_vertex_identity(GraphScanState * node, int dest_elem_idx,
							  GraphHopFrame * hop)
{
	int			gid;
	int			first_pi;

	if (dest_elem_idx < 0 || dest_elem_idx >= node->num_pi)
		return true;
	gid = node->samevar_group[dest_elem_idx];
	if (gid < 0)
		return true;
	if (node->samevar_elem_kind[gid] != 'v')
		return true;

	first_pi = node->samevar_first_pi[gid];

	if (dest_elem_idx == first_pi && first_pi != 0)
	{
		/*
		 * First occurrence at a non-zero position: store PK values from the
		 * hop's to_vertex for future comparisons.
		 */
		int			nkeys = hop->nto;
		int			ki;

		node->samevar_nkeys[gid] = nkeys;
		if (nkeys > 0)
		{
			if (node->samevar_key_values[gid])
				pfree(node->samevar_key_values[gid]);
			if (node->samevar_key_nulls[gid])
				pfree(node->samevar_key_nulls[gid]);

			node->samevar_key_values[gid] = (Datum *)
				palloc(sizeof(Datum) * nkeys);
			node->samevar_key_nulls[gid] = (bool *)
				palloc(sizeof(bool) * nkeys);

			for (ki = 0; ki < nkeys; ki++)
			{
				node->samevar_key_values[gid][ki] =
					hop->to_vertex_values[ki];
				node->samevar_key_nulls[gid][ki] =
					hop->to_vertex_nulls[ki];
			}
		}
		return true;
	}
	else
	{
		/* Subsequent occurrence: compare against stored reference */
		int			nkeys = node->samevar_nkeys[gid];
		int			ki;

		for (ki = 0; ki < nkeys; ki++)
		{
			bool		ref_null;

			if (ki >= hop->nto)
				return false;

			ref_null = (node->samevar_key_nulls[gid] ?
						node->samevar_key_nulls[gid][ki] : false);
			if (hop->to_vertex_nulls[ki] != ref_null)
				return false;
			if (!hop->to_vertex_nulls[ki] &&
				DatumGetInt32(hop->to_vertex_values[ki]) !=
				DatumGetInt32(node->samevar_key_values[gid][ki]))
				return false;
		}
		return true;
	}
}

/* ----------------------------------------------------------------
 *	 hop_close_edge_scan
 * ----------------------------------------------------------------
 */
static void
hop_close_edge_scan(GraphHopFrame * hop)
{
	if (hop->edge_scan)
	{
		table_endscan(hop->edge_scan);
		hop->edge_scan = NULL;
	}
	if (hop->edge_slot)
	{
		ExecDropSingleTupleTableSlot(hop->edge_slot);
		hop->edge_slot = NULL;
	}
}

/* ----------------------------------------------------------------
 *	 hop_reset
 * ----------------------------------------------------------------
 */
static void
hop_reset(GraphHopFrame * hop)
{
	hop_close_edge_scan(hop);
	if (hop->to_vertex_slot)
	{
		ExecDropSingleTupleTableSlot(hop->to_vertex_slot);
		hop->to_vertex_slot = NULL;
	}
	if (hop->to_vertex_values)
	{
		pfree(hop->to_vertex_values);
		hop->to_vertex_values = NULL;
	}
	if (hop->to_vertex_nulls)
	{
		pfree(hop->to_vertex_nulls);
		hop->to_vertex_nulls = NULL;
	}
	hop->nto = 0;
	if (hop->from_vertex_values)
	{
		pfree(hop->from_vertex_values);
		hop->from_vertex_values = NULL;
	}
	if (hop->from_vertex_nulls)
	{
		pfree(hop->from_vertex_nulls);
		hop->from_vertex_nulls = NULL;
	}
	hop->nfrom = 0;

	/*
	 * Don't drop from_vertex_slot. It is owned by the previous hop or by the
	 * start vertex state.
	 */
	hop->from_vertex_slot = NULL;
	hop->from_vertex_elemid = InvalidOid;
	hop->undirected_has_seen = false;
	/* VLE stack fields managed by hop_advance / hop_reset */
	hop->vle_active = false;
	hop->vle_lower = 0;
	hop->vle_upper = 0;
	hop->vle_stack_top = -1;
	if (hop->vle_stack_values)
	{
		for (int vi = 0; vi < hop->vle_stack_capacity; vi++)
		{
			if (hop->vle_stack_values[vi])
				pfree(hop->vle_stack_values[vi]);
			if (hop->vle_stack_nulls[vi])
				pfree(hop->vle_stack_nulls[vi]);
			if (hop->vle_stack_edge_scan[vi])
				table_endscan(hop->vle_stack_edge_scan[vi]);
			if (hop->vle_stack_edge_slot[vi])
				ExecDropSingleTupleTableSlot(hop->vle_stack_edge_slot[vi]);
		}
		pfree(hop->vle_stack_values);
		pfree(hop->vle_stack_nulls);
		pfree(hop->vle_stack_elemid);
		pfree(hop->vle_stack_nkeys);
		pfree(hop->vle_stack_table_idx);
		pfree(hop->vle_stack_scan_pass);
		pfree(hop->vle_stack_edge_scan);
		pfree(hop->vle_stack_edge_slot);
		hop->vle_stack_values = NULL;
		hop->vle_stack_nulls = NULL;
		hop->vle_stack_elemid = NULL;
		hop->vle_stack_nkeys = NULL;
		hop->vle_stack_table_idx = NULL;
		hop->vle_stack_scan_pass = NULL;
		hop->vle_stack_edge_scan = NULL;
		hop->vle_stack_edge_slot = NULL;
	}
	hop->vle_stack_capacity = 0;
}

/*
 * Initialize a GraphHopFrame with the given source vertex context.
 * All edge-scan state is reset to 0.  The from_vertex_slot is borrowed
 * (caller owns it), not copied.
 */
static void
hop_init(GraphHopFrame * hop, int nfrom, Datum *from_values,
		 bool *from_nulls, TupleTableSlot *from_vertex_slot,
		 Oid from_vertex_elemid)
{
	int			ki;

	hop->nfrom = nfrom;
	if (hop->from_vertex_values)
		pfree(hop->from_vertex_values);
	if (hop->from_vertex_nulls)
		pfree(hop->from_vertex_nulls);
	hop->from_vertex_values = (Datum *) palloc(sizeof(Datum) * nfrom);
	hop->from_vertex_nulls = (bool *) palloc(sizeof(bool) * nfrom);
	for (ki = 0; ki < nfrom; ki++)
	{
		hop->from_vertex_values[ki] = from_values[ki];
		hop->from_vertex_nulls[ki] = from_nulls[ki];
	}
	hop->from_vertex_slot = from_vertex_slot;
	hop->from_vertex_elemid = from_vertex_elemid;
	hop_close_edge_scan(hop);
	hop->edge_table_index = 0;
	hop->scan_pass = 0;

	/*
	 * Don't drop to_vertex_slot here. It may be reused as from_vertex_slot by
	 * the caller (e.g., VLE re-init).
	 */
	hop->undirected_has_seen = false;

	/*
	 * VLE stack fields are NOT reset here. They are managed by hop_advance
	 * and hop_reset.
	 */
}

/* ----------------------------------------------------------------
 *	 ExtractTargetVertexKeys
 *
 *		Extract the target vertex PK column values from an edge tuple.
 * ----------------------------------------------------------------
 */
static void
ExtractTargetVertexKeys(TupleTableSlot *edge_slot,
						GraphElementRelInfo * edge_info,
						EdgeDirection dir, int scan_pass,
						int nkeys, Datum *values, bool *nulls)
{
	int			ki;
	AttrNumber *attnums;
	int			n_fk;

	if (edge_is_incoming(dir, scan_pass))
	{
		n_fk = edge_info->nsrc_fk_attnums;
		attnums = edge_info->src_fk_attnums;
	}
	else
	{
		n_fk = edge_info->ndst_fk_attnums;
		attnums = edge_info->dst_fk_attnums;
	}

	if (nkeys != n_fk)
		return;

	for (ki = 0; ki < n_fk; ki++)
		values[ki] = slot_getattr(edge_slot, attnums[ki], &nulls[ki]);
}

/*
 * Find the vertex element index in the catalog that matches the
 * destination vertex of the given edge element.
 */
static int
find_vertex_for_edge_dest(GraphScanState * node, int edge_elem_idx)
{
	Oid			dest_elemoid;
	int			i;

	if (edge_elem_idx < 0 || edge_elem_idx >= node->num_elements)
		return -1;

	dest_elemoid = node->element_info[edge_elem_idx].dest_vertex_elemid;

	for (i = 0; i < node->num_elements; i++)
	{
		if (node->element_info[i].element_kind != 'v')
			continue;
		if (node->element_info[i].element_oid == dest_elemoid)
			return i;
	}
	return -1;
}

/*
 * Find the vertex element index whose element OID matches the
 * SOURCE vertex of the given edge.
 */
static int
find_vertex_for_edge_src(GraphScanState * node, int edge_elem_idx)
{
	Oid			src_elemoid;
	int			i;

	if (edge_elem_idx < 0 || edge_elem_idx >= node->num_elements)
		return -1;

	src_elemoid = node->element_info[edge_elem_idx].src_vertex_elemid;

	for (i = 0; i < node->num_elements; i++)
	{
		if (node->element_info[i].element_kind != 'v')
			continue;
		if (node->element_info[i].element_oid == src_elemoid)
			return i;
	}
	return -1;
}

/*
 * fetch_vertex_by_pk
 *
 *		Look up a vertex tuple by primary key values. Uses a sequential scan
 *		of the vertex table.
 *		Returns a materialized TupleTableSlot, or NULL if not found.
 *		Caller is responsible for freeing the slot.
 */
static TupleTableSlot *
fetch_vertex_by_pk(GraphElementRelInfo * vinfo, Datum *pk_values,
				   bool *pk_nulls, int npk,
				   EState *estate)
{
	TupleTableSlot *slot;
	TableScanDesc scan;

	if (npk == 0 || vinfo->nkey_attnums != npk)
		return NULL;

	slot = MakeSingleTupleTableSlot(vinfo->tupdesc,
									table_slot_callbacks(vinfo->heap_rel));
	scan = table_beginscan(vinfo->heap_rel, estate->es_snapshot,
						   0, NULL, SO_NONE);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		match = true;
		int			ki;

		for (ki = 0; ki < npk; ki++)
		{
			bool		isnull;
			Datum		col_val;

			col_val = slot_getattr(slot, vinfo->key_attnums[ki], &isnull);
			if (isnull || pk_nulls[ki] ||
				DatumGetInt32(col_val) != DatumGetInt32(pk_values[ki]))
			{
				match = false;
				break;
			}
		}
		if (match)
		{
			ExecMaterializeSlot(slot);
			table_endscan(scan);
			return slot;
		}
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	return NULL;
}

/*
 * Fill one attribute of a tuple slot from the column mapping.
 * ci is the column mapping index, slot_att is the 0-based attribute
 * position in the target slot.  For constants, reads col_const_value.
 * For Var-based columns, reads from the appropriate element slot via
 * slot_for_elem cache.
 */
static void
fill_one_slot_attr(GraphScanState * node, TupleTableSlot *slot,
				   int slot_att, int ci)
{
	if (node->col_is_const[ci])
	{
		slot->tts_values[slot_att] = node->col_const_value[ci];
		slot->tts_isnull[slot_att] = node->col_const_null[ci];
	}
	else
	{
		int			pi = node->col_elem_index[ci];

		if (pi >= 0 && pi < node->num_pi &&
			node->slot_for_elem[pi] != NULL)
		{
			TupleTableSlot *elem_slot = node->slot_for_elem[pi];

			if (node->col_propid[ci] != InvalidOid &&
				(node->col_prop_unsafe[ci] ||
				 find_attnum_by_name(elem_slot, node->col_propname[ci]) == InvalidAttrNumber))
			{
				/* Computed or label-unsafe property, resolve at runtime */
				GraphElementPattern *gep = GetPatternElement(node, pi);
				bool		isnull;

				if (gep && IS_EDGE_PATTERN(gep->kind))
				{
					int			hi = (pi - 1) / 2;

					slot->tts_values[slot_att] =
						resolve_graph_property(node, hi,
											   node->col_propid[ci],
											   elem_slot, &isnull, true);
				}
				else
				{
					slot->tts_values[slot_att] =
						resolve_graph_property(node, pi,
											   node->col_propid[ci],
											   elem_slot, &isnull, false);
				}
				slot->tts_isnull[slot_att] = isnull;
			}
			else
			{
				AttrNumber	resolved_att = find_attnum_by_name(
															   elem_slot, node->col_propname[ci]);

				if (resolved_att != InvalidAttrNumber)
				{
					bool		isnull;

					slot->tts_values[slot_att] =
						slot_getattr(elem_slot, resolved_att, &isnull);
					slot->tts_isnull[slot_att] = isnull;
				}
				else
				{
					slot->tts_values[slot_att] = (Datum) 0;
					slot->tts_isnull[slot_att] = true;
				}
			}
		}
		else
		{
			slot->tts_values[slot_att] = (Datum) 0;
			slot->tts_isnull[slot_att] = true;
		}
	}
}

/*
 * MaterializeCurrentPathSlot
 *
 * Fill the result tuple slot with values from the active path,
 * using the column mapping built during ExecInitGraphScan.
 * Returns the slot.
 */
static TupleTableSlot *
MaterializeCurrentPathSlot(GraphScanState * node)
{
	update_slot_cache(node);

	if (node->ss.ps.ps_ProjInfo != NULL)
	{
		TupleTableSlot *scan_slot = node->ss.ss_ScanTupleSlot;
		int			scan_natts = scan_slot->tts_tupleDescriptor->natts;
		int			ci;

		ExecClearTuple(scan_slot);

		/* Initialize all scan slot values to NULL (clear stale data) */
		{
			int			ai;

			for (ai = 0; ai < scan_natts; ai++)
			{
				scan_slot->tts_values[ai] = (Datum) 0;
				scan_slot->tts_isnull[ai] = true;
			}
		}

		/*
		 * Populate ALL scan slot attributes from the column mapping.
		 * ExecProject's compiled steps use EEOP_SCAN_VAR to read from the
		 * scan slot, so every attribute must be filled regardless of whether
		 * the outer query's target list references it as a bare Var or wraps
		 * it in a FuncExpr/RelabelType/etc.
		 */
		for (ci = 0; ci < scan_natts; ci++)
			fill_one_slot_attr(node, scan_slot, ci, ci);

		/*
		 * For columns that have a compiled expression (e.g., FuncExpr
		 * wrapping a GraphPropertyRef), evaluate the expression with the
		 * element slot as ecxt_outertuple and store the result into the scan
		 * slot, overriding the raw property value.
		 */
		{
			ExprContext *econtext = node->ss.ps.ps_ExprContext;

			for (ci = 0; ci < scan_natts; ci++)
			{
				if (node->col_exprs &&
					ci < node->ncols &&
					node->col_exprs[ci] != NULL)
				{
					int			pi = node->col_expr_elem_index[ci];
					TupleTableSlot *elem_slot = NULL;

					if (pi >= 0 && pi < node->num_pi)
						elem_slot = node->slot_for_elem[pi];

					if (elem_slot)
					{
						econtext->ecxt_outertuple = elem_slot;
						scan_slot->tts_values[ci] =
							ExecEvalExpr(node->col_exprs[ci],
										 econtext,
										 &scan_slot->tts_isnull[ci]);
					}
				}
			}
		}

		scan_slot->tts_nvalid = scan_natts;
		ExecStoreVirtualTuple(scan_slot);

		{
			ExprContext *econtext = node->ss.ps.ps_ExprContext;

			econtext->ecxt_scantuple = scan_slot;
			return ExecProject(node->ss.ps.ps_ProjInfo);
		}
	}
	else
	{
		TupleTableSlot *slot = node->ss.ps.ps_ResultTupleSlot;
		int			ci;
		int			ncols;

		/*
		 * Use the scan tuple descriptor's column count, not the plan target
		 * list's.  When the GraphScan is inside a subquery, the outer plan
		 * may pass an empty target list (e.g., for count(*)), but the scan
		 * still needs to materialize its full column set so that ExecQual can
		 * read Var(varno=scanrelid) references.
		 */
		ncols = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor ?
			node->ss.ss_ScanTupleSlot->tts_tupleDescriptor->natts : 0;

		ExecClearTuple(slot);

		for (ci = 0; ci < ncols; ci++)
			fill_one_slot_attr(node, slot, ci, ci);

		slot->tts_nvalid = ncols;
		ExecStoreVirtualTuple(slot);
		return slot;
	}
}

/* ----------------------------------------------------------------
 *	 vle_push_frame
 *
 *		Push a vertex state onto the VLE DFS stack at the next depth.
 *		The key values are copied into palloc'd storage owned by the
 *		stack frame.  Does NOT modify edge_table_index/scan_pass in
 *		the frame (those are set when the frame's edge scan begins).
 * ----------------------------------------------------------------
 */
static void
vle_push_frame(GraphHopFrame * hop, Oid elem_oid, int nkeys,
			   Datum *values, bool *nulls)
{
	int			top = hop->vle_stack_top + 1;
	int			ki;

	Assert(top < hop->vle_stack_capacity);

	hop->vle_stack_elemid[top] = elem_oid;
	hop->vle_stack_nkeys[top] = nkeys;
	hop->vle_stack_values[top] = (Datum *) palloc(sizeof(Datum) * nkeys);
	hop->vle_stack_nulls[top] = (bool *) palloc(sizeof(bool) * nkeys);
	for (ki = 0; ki < nkeys; ki++)
	{
		hop->vle_stack_values[top][ki] = values[ki];
		hop->vle_stack_nulls[top][ki] = nulls[ki];
	}
	/* Resume position: start fresh scan from this vertex */
	hop->vle_stack_table_idx[top] = 0;
	hop->vle_stack_scan_pass[top] = 0;

	hop->vle_stack_top = top;
}

/* ----------------------------------------------------------------
 *	 vle_pop_frame
 *
 *		Pop the top frame from the VLE DFS stack, freeing its storage.
 * ----------------------------------------------------------------
 */
static void
vle_pop_frame(GraphHopFrame * hop)
{
	int			top = hop->vle_stack_top;

	Assert(top >= 0);
	if (hop->vle_stack_edge_scan[top])
	{
		table_endscan(hop->vle_stack_edge_scan[top]);
		hop->vle_stack_edge_scan[top] = NULL;
	}
	if (hop->vle_stack_edge_slot[top])
	{
		ExecDropSingleTupleTableSlot(hop->vle_stack_edge_slot[top]);
		hop->vle_stack_edge_slot[top] = NULL;
	}
	pfree(hop->vle_stack_values[top]);
	pfree(hop->vle_stack_nulls[top]);
	hop->vle_stack_values[top] = NULL;
	hop->vle_stack_nulls[top] = NULL;
	hop->vle_stack_top--;
}

/* ----------------------------------------------------------------
 *	 vle_activate
 *
 *		Allocate and initialize the VLE DFS stack for the given hop,
 *		and push the hop's current from_vertex as depth 0.
 * ----------------------------------------------------------------
 */
static void
vle_activate(GraphScanState * node, GraphHopFrame * hop,
			 GraphElementPattern *edge_elem)
{
	List	   *quant = edge_elem->quantifier;
	int			lower;
	int			upper;
	int			capacity;

	Assert(list_length(quant) == 2);
	lower = linitial_int(quant);
	upper = lsecond_int(quant);
	if (upper == -1)
		upper = max_vle_stack_depth; /* unbounded, limit with max stack depth */

	capacity = upper + 1;

	hop->vle_lower = lower;
	hop->vle_upper = upper;
	hop->vle_stack_capacity = capacity;
	hop->vle_stack_elemid = (Oid *) palloc(sizeof(Oid) * capacity);
	hop->vle_stack_nkeys = (int *) palloc(sizeof(int) * capacity);
	hop->vle_stack_values = (Datum **) palloc(sizeof(Datum *) * capacity);
	hop->vle_stack_nulls = (bool **) palloc(sizeof(bool *) * capacity);
	hop->vle_stack_table_idx = (int *) palloc(sizeof(int) * capacity);
	hop->vle_stack_scan_pass = (int *) palloc(sizeof(int) * capacity);
	hop->vle_stack_edge_scan = (struct TableScanDescData **) palloc(sizeof(struct TableScanDescData *) * capacity);
	hop->vle_stack_edge_slot = (TupleTableSlot **) palloc(sizeof(TupleTableSlot *) * capacity);
	hop->vle_stack_top = -1;

	/* All frame pointers start NULL so vle_pop_frame after error is safe */
	memset(hop->vle_stack_values, 0, sizeof(Datum *) * capacity);
	memset(hop->vle_stack_nulls, 0, sizeof(bool *) * capacity);
	memset(hop->vle_stack_edge_scan, 0, sizeof(struct TableScanDescData *) * capacity);
	memset(hop->vle_stack_edge_slot, 0, sizeof(TupleTableSlot *) * capacity);

	/* Push the current source vertex at depth 0 */
	vle_push_frame(hop, hop->from_vertex_elemid,
				   hop->nfrom, hop->from_vertex_values,
				   hop->from_vertex_nulls);

	hop->vle_active = true;
}

/* ----------------------------------------------------------------
 *	 hop_advance
 *
 *		Try to advance the DFS chain from hop hi by finding the next
 *		matching edge for that hop.  Returns true if the chain made
 *		progress (a new path segment was found), false if the current
 *		hop is exhausted and we must backtrack.
 *
 *		On success, if hi is the last hop, *path_complete is set to
 *		true and the result path is materialized in the scan slot.
 *		If there are more hops ahead, the next hop is initialized.
 *
 *		For VLE hops, this function implements a DFS-with-stack:
 *		- When vle_active is false, first-call logic activates VLE.
 *		- When vle_active is true, the stack guides DFS.
 *		- Returns false only when the entire stack is exhausted.
 * ----------------------------------------------------------------
 */
static bool
hop_advance(GraphScanState * node, int hi, bool *path_complete,
			bool *vle_continue)
{
	GraphHopFrame *hop = &node->hops[hi];
	int			edge_elem_idx = 2 * hi + 1;
	GraphElementPattern *edge_elem = GetPatternElement(node, edge_elem_idx);
	EdgeDirection dir = GRAPH_DIR_OUTGOING;
	bool		is_vle_hop = false;

	*path_complete = false;
	*vle_continue = false;

	/* Determine direction from the pattern element */
	if (edge_elem)
	{
		switch (edge_elem->kind)
		{
			case EDGE_PATTERN_LEFT:
				dir = GRAPH_DIR_INCOMING;
				break;
			case EDGE_PATTERN_ANY:
				dir = GRAPH_DIR_UNDIRECTED;
				break;
			default:
				dir = GRAPH_DIR_OUTGOING;
				break;
		}
	}

	/*
	 * Determine if this is a VLE hop (has a quantifier other than the
	 * implicit {1,1}).  Only the last hop can be VLE for now; intermediate
	 * hops with quantifiers are not supported.
	 */
	if (hi + 1 >= node->n_hops &&
		edge_elem && edge_elem->quantifier != NULL &&
		(linitial_int(edge_elem->quantifier) != 1 ||
		 lsecond_int(edge_elem->quantifier) != 1))
		is_vle_hop = true;

	/* --- Path 1: Not VLE (implicit {1,1}, or intermediate hop) --- */
	if (!is_vle_hop && !hop->vle_active)
	{
		if (!hop_scan_edges(node, hop, edge_elem_idx, dir))
			return false;

		/* Found a matching (edge, dest_vertex) */

		if (hi + 1 < node->n_hops)
		{
			/* More hops ahead, initialize the next one */
			hop_init(&node->hops[hi + 1], hop->nto,
					 hop->to_vertex_values, hop->to_vertex_nulls,
					 hop->to_vertex_slot, hop->to_vertex_elemid);
			return true;
		}

		/* Last hop / path is complete */
		*path_complete = true;
		return true;
	}

	/* --- Path 2: VLE hop --- */

	/* First call for this VLE hop: activate the stack */
	if (!hop->vle_active)
	{
		/* Push the current source vertex at depth 0 and activate */
		vle_activate(node, hop, edge_elem);

		/*
		 * For {0,N}: depth 0 itself is a valid result (zero hops). Yield the
		 * start vertex before scanning any edges.
		 */
		if (0 >= hop->vle_lower)
		{
			*path_complete = true;
			*vle_continue = (0 < hop->vle_upper);
			return true;
		}
	}

	/*
	 * DFS loop: scan edges from the current frame's source vertex. We loop on
	 * backtrack: when a frame is exhausted, pop and try the parent frame's
	 * next sibling edge.
	 *
	 * Each frame owns its own edge_scan/edge_slot, kept alive in the
	 * vle_stack_edge_scan/vle_stack_edge_slot arrays.  We swap them into
	 * hop->edge_scan/edge_slot before calling vle_find_next_edge (which
	 * operates on the hop-level fields), then swap back so the frame retains
	 * ownership.
	 */
	for (;;)
	{
		int			frame_idx = hop->vle_stack_top;

		Assert(frame_idx >= 0);

		/*
		 * If this frame is already at max depth (>= vle_upper), it cannot go
		 * deeper.  Pop it and continue backtracking.
		 */
		if (frame_idx >= hop->vle_upper)
		{
			vle_pop_frame(hop);
			if (hop->vle_stack_top < 0)
			{
				hop->vle_active = false;
				hop->vle_lower = 0;
				hop->vle_upper = 0;
				hop->vle_stack_capacity = 0;
				return false;
			}
			continue;
		}

		/*
		 * Set the hop's from_vertex to this frame's vertex so
		 * vle_find_next_edge knows the source.
		 */
		{
			int			f_nkeys = hop->vle_stack_nkeys[frame_idx];

			if (hop->nfrom != f_nkeys)
			{
				if (hop->from_vertex_values)
					pfree(hop->from_vertex_values);
				if (hop->from_vertex_nulls)
					pfree(hop->from_vertex_nulls);
				hop->from_vertex_values =
					(Datum *) palloc(sizeof(Datum) * f_nkeys);
				hop->from_vertex_nulls =
					(bool *) palloc(sizeof(bool) * f_nkeys);
				hop->nfrom = f_nkeys;
			}
			memcpy(hop->from_vertex_values,
				   hop->vle_stack_values[frame_idx],
				   sizeof(Datum) * f_nkeys);
			memcpy(hop->from_vertex_nulls,
				   hop->vle_stack_nulls[frame_idx],
				   sizeof(bool) * f_nkeys);
			hop->from_vertex_elemid = hop->vle_stack_elemid[frame_idx];
		}

		/*
		 * Swap in this frame's own edge scan and restore its resume position.
		 * Since each frame owns its own scan, an in-progress scan continues
		 * exactly where it left off.
		 */
		hop->edge_scan = hop->vle_stack_edge_scan[frame_idx];
		hop->edge_slot = hop->vle_stack_edge_slot[frame_idx];
		hop->edge_table_index = hop->vle_stack_table_idx[frame_idx];
		hop->scan_pass = hop->vle_stack_scan_pass[frame_idx];

		/*
		 * Find the next edge from the current source vertex, using the
		 * frame's scan (or starting fresh if NULL).
		 */
		if (vle_find_next_edge(node,
							   hop->from_vertex_elemid,
							   hop->nfrom, hop->from_vertex_values,
							   hop->from_vertex_nulls,
							   &hop->edge_table_index, &hop->scan_pass,
							   edge_elem_idx, dir,
							   hop,
							   &hop->to_vertex_elemid,
							   &hop->nto, &hop->to_vertex_values,
							   &hop->to_vertex_nulls))
		{
			int			new_depth;
			bool		result_found;
			bool		continue_deeper;

			/*
			 * Found an edge.  Swap back: the frame keeps ownership of its
			 * scan (now positioned after this edge).
			 */
			hop->vle_stack_edge_scan[frame_idx] = hop->edge_scan;
			hop->vle_stack_edge_slot[frame_idx] = hop->edge_slot;
			hop->vle_stack_table_idx[frame_idx] = hop->edge_table_index;
			hop->vle_stack_scan_pass[frame_idx] = hop->scan_pass;
			hop->edge_scan = NULL;
			hop->edge_slot = NULL;

			/*
			 * Push the destination vertex onto the stack at the next depth
			 * level.
			 */
			vle_push_frame(hop, hop->to_vertex_elemid,
						   hop->nto, hop->to_vertex_values,
						   hop->to_vertex_nulls);
			new_depth = hop->vle_stack_top;

			/* Determine if this depth is a valid result */
			result_found = (new_depth >= hop->vle_lower);
			continue_deeper = (new_depth < hop->vle_upper);

			if (result_found)
			{
				/* Yield this result */
				*path_complete = true;
			}

			if (continue_deeper)
			{
				/*
				 * Keep exploring deeper from this destination vertex. Its
				 * scan is NULL (vle_push_frame zeros it), so the next call
				 * will open a fresh scan.
				 */
				*vle_continue = true;
			}
			else
			{
				/*
				 * At max depth: pop this frame since we won't explore deeper.
				 * Its vertex may have been yielded (if within lower bound).
				 * vle_pop_frame closes/frees its scan. If parent frames
				 * remain, signal the caller to continue DFS for sibling
				 * edges.
				 */
				vle_pop_frame(hop);
				if (hop->vle_stack_top >= 0)
					*vle_continue = true;
			}

			return true;
		}

		/*
		 * No more edges from the current source vertex.  Swap back (the scan
		 * may have been closed by vle_find_next_edge during table
		 * exhaustion), then pop this frame.
		 */
		hop->vle_stack_edge_scan[frame_idx] = hop->edge_scan;
		hop->vle_stack_edge_slot[frame_idx] = hop->edge_slot;
		hop->edge_scan = NULL;
		hop->edge_slot = NULL;

		vle_pop_frame(hop);

		if (hop->vle_stack_top < 0)
		{
			/* Entire stack exhausted. VLE hop is done */
			hop->vle_active = false;
			hop->vle_lower = 0;
			hop->vle_upper = 0;
			hop->vle_stack_capacity = 0;
			return false;
		}

		/* Back to parent frame. The loop will swap in its scan next iteration */
	}

	/* NOTREACHED */
	return false;
}

/* ----------------------------------------------------------------
 *	 hop_backtrack
 *
 *		Clean up the hop at index hi and return to the previous hop.
 *		If the previous hop exists, its to_vertex_slot is released
 *		(that destination is no longer reachable).
 *		Returns the new hop index (-1 if no more hops to backtrack to).
 * ----------------------------------------------------------------
 */
static int
hop_backtrack(GraphScanState * node, int hi)
{
	hop_reset(&node->hops[hi]);
	hi--;

	if (hi >= 0)
	{
		/* Release the stale destination of the previous hop */
		GraphHopFrame *prev = &node->hops[hi];

		if (prev->to_vertex_slot)
		{
			ExecDropSingleTupleTableSlot(prev->to_vertex_slot);
			prev->to_vertex_slot = NULL;
		}
	}
	return hi;
}

/* ----------------------------------------------------------------
 *	 ExecGraphScan
 *
 *		Core hop-based DFS state machine.
 *
 *		State machine phases:
 *		  PHASE_START_VERTEX:	fetch the next start vertex
 *		  PHASE_EDGE_SCAN:		try to advance the current hop chain;
 *								on success, if path is complete, return it;
 *								on exhaustion, backtrack or fetch next start
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecGraphScan(PlanState *pstate)
{
	GraphScanState *node = castNode(GraphScanState, pstate);
	int			hi;

	if (!node->is_initialized)
	{
		init_start_vertex(node);
		node->is_initialized = true;
	}

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		/* Prevent runaway DFS: count total path attempts */
		if (++node->scan_attempts > max_vle_stack_depth)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("exceeded maximum scan attempts in graph traversal"),
					 errhint("The query may contain a cyclic pattern. "
							 "Try simplifying the pattern or adding more restrictive "
							 "label constraints.")));

		/* --- Phase: start vertex --- */
		if (node->current_hop == -1)
		{
			/* Fetch next start vertex; return NULL when all are exhausted */
			if (!start_vertex_advance(node))
				return NULL;

			node->slot_for_elem[0] = node->start_vertex_slot;

			/* Vertex-only pattern: materialize and return */
			if (node->n_hops == 0)
			{
				TupleTableSlot *result = MaterializeCurrentPathSlot(node);

				if (result &&
					evaluate_graph_where(node, node->where_clause))
				{
					ExprContext *econtext = node->ss.ps.ps_ExprContext;

					/*
					 * Use the scan slot for ecxt_scantuple, not the result
					 * slot.  MaterializeCurrentPathSlot may return
					 * ps_ResultTupleSlot (via ExecProject) which could have
					 * fewer columns than the scan slot when the outer plan
					 * doesn't need all graph columns (e.g., count(*)
					 * subquery). Var(scanrelid, attno) in plan->qual reads
					 * from ecxt_scantuple.
					 */
					econtext->ecxt_scantuple = node->ss.ss_ScanTupleSlot;

					if (ExecQual(node->ss.ps.qual, econtext))
						return result;
				}
				continue;
			}

			/* Initialize hop 0 with the new start vertex */
			{
				GraphElementPattern *elem0 = GetPatternElement(node, 0);
				GraphElementRelInfo *rel0 =
					GetElementRelInfo(node, elem0, node->start_table_index);

				hop_init(&node->hops[0], node->n_start_keys,
						 node->start_vertex_values,
						 node->start_vertex_nulls,
						 node->start_vertex_slot,
						 rel0 ? rel0->element_oid : InvalidOid);
			}

			node->current_hop = 0;
		}

		/* --- Phase: DFS edge scan / backtrack --- */
		hi = node->current_hop;

		while (hi >= 0 && hi < node->n_hops)
		{
			bool		path_complete;
			bool		vle_continue;

			if (hop_advance(node, hi, &path_complete, &vle_continue))
			{
				if (path_complete)
				{
					TupleTableSlot *result;

					/* Complete path found, materialize */
					node->current_hop = hi;
					result = MaterializeCurrentPathSlot(node);

					/* Check the outer GraphPattern WHERE clause */
					if (!evaluate_graph_where(node, node->where_clause))
						continue;

					/*
					 * Check the plan-level qual (outer Var refs from LATERAL
					 * joins, pushed-down quals, etc.)
					 */
					{
						ExprContext *econtext = node->ss.ps.ps_ExprContext;

						econtext->ecxt_scantuple =
							node->ss.ss_ScanTupleSlot;
						if (!ExecQual(node->ss.ps.qual, econtext))
							continue;
					}

					return result;
				}

				if (!vle_continue)
				{
					/* Advance to the next hop in the chain */
					hi++;
					node->current_hop = hi;
				}

				/*
				 * If vle_continue is true, stay on the same hop. The VLE
				 * stack in hop_advance handles continuation.
				 */
			}
			else
			{
				/* Current hop exhausted, backtrack */
				hi = hop_backtrack(node, hi);
				node->current_hop = hi;
			}
		}

		/* All hops exhausted for the current start vertex */
		{
			int			i;

			for (i = 0; i < node->n_hops; i++)
				hop_reset(&node->hops[i]);
		}
		node->current_hop = -1;
	}
}

/* ----------------------------------------------------------------
 *	 ExecEndGraphScan
 * ----------------------------------------------------------------
 */
void
ExecEndGraphScan(GraphScanState * node)
{
	int			i;

	element_info_cleanup(node);

	if (node->start_scan)
	{
		table_endscan(node->start_scan);
		node->start_scan = NULL;
	}

	if (node->start_vertex_slot)
	{
		ExecDropSingleTupleTableSlot(node->start_vertex_slot);
		node->start_vertex_slot = NULL;
	}

	if (node->start_vertex_values)
		pfree(node->start_vertex_values);
	if (node->start_vertex_nulls)
		pfree(node->start_vertex_nulls);

	if (node->hops)
	{
		for (i = 0; i < node->n_hops; i++)
			hop_reset(&node->hops[i]);
		pfree(node->hops);
	}

	if (node->element_quals)
		pfree(node->element_quals);
	if (node->col_elem_index)
		pfree(node->col_elem_index);
	if (node->col_is_const)
		pfree(node->col_is_const);
	if (node->col_const_value)
		pfree(node->col_const_value);
	if (node->col_const_null)
		pfree(node->col_const_null);
	if (node->col_propname)
		pfree(node->col_propname);
	if (node->col_propid)
		pfree(node->col_propid);
	if (node->col_prop_unsafe)
		pfree(node->col_prop_unsafe);
	if (node->slot_for_elem)
		pfree(node->slot_for_elem);

	if (node->graph_cxt)
		MemoryContextDelete(node->graph_cxt);
}

/* ----------------------------------------------------------------
 *	 ExecReScanGraphScan
 * ----------------------------------------------------------------
 */
void
ExecReScanGraphScan(GraphScanState * node)
{
	int			i;

	if (node->start_scan)
	{
		table_endscan(node->start_scan);
		node->start_scan = NULL;
	}

	if (node->start_vertex_slot)
	{
		ExecDropSingleTupleTableSlot(node->start_vertex_slot);
		node->start_vertex_slot = NULL;
	}

	if (node->start_vertex_values)
	{
		pfree(node->start_vertex_values);
		node->start_vertex_values = NULL;
	}
	if (node->start_vertex_nulls)
	{
		pfree(node->start_vertex_nulls);
		node->start_vertex_nulls = NULL;
	}
	node->n_start_keys = 0;

	if (node->hops)
	{
		for (i = 0; i < node->n_hops; i++)
			hop_reset(&node->hops[i]);
	}

	node->current_hop = -1;
	node->is_initialized = false;

	UpdateChangedParamSet(&node->ss.ps, node->ss.ps.chgParam);
}
