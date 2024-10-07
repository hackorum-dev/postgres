/*-------------------------------------------------------------------------
 *
 * paths.h
 *	  prototypes for various files in optimizer/path
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/paths.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PATHS_H
#define PATHS_H

#include "nodes/pathnodes.h"

/*
 * Scan strategy advice.
 *
 * If SSA_CONSIDER_INDEXONLY is not set, index-only scan paths will not even
 * be generated, and we'll generated index-scan paths for the same cases
 * instead. If any other bit is not set, paths of that type will still be
 * generated but will be marked as disabled.
 *
 * So, if you want to avoid an index-only scan, you can either unset
 * SSA_CONSIDER_INDEXONLY (in which case you'll get an index-scan instead,
 * which may end up disabled if you also unset SSA_INDEXSCAN) or you can
 * unset SSA_INDEXONLYSCAN (in which the index-only scan will be disabled
 * and the cheapest non-disabled alternative, if any, will be chosen, but
 * no corresponding index scan will be considered). If, on the other hand,
 * you want to encourage an index-only scan, you can set just SSA_INDEXONLYSCAN
 * and SSA_CONSIDER_INDEXONLY and clear all of the other bits.
 *
 * A default scan strategy advice mask is stored in the PlannerGlobal object
 * based on the values of the various enable_* GUCs. This value is propagted
 * into each RelOptInfo for a baserel, and from baserels to their inheritance
 * children when partitions are expanded. In either case, the value can be
 * usefully changed in get_relation_info_hook.
 */
#define SSA_TIDSCAN						0x0001
#define SSA_SEQSCAN						0x0002
#define SSA_INDEXSCAN					0x0004
#define SSA_INDEXONLYSCAN				0x0008
#define SSA_BITMAPSCAN					0x0010
#define SSA_CONSIDER_INDEXONLY			0x0020

/*
 * Join strategy advice.
 *
 * Paths that don't match the join strategy advice will be either be disabled
 * or will not be generated in the first place. It's only permissible to skip
 * generating a path if doing so can't result in planner failure. The initial
 * mask is computed on the basis of the various enable_* GUCs, and can be
 * overriden by hooks.
 *
 * We have five main join strategies: a foreign join (when supported by the
 * relevant FDW), a merge join, a nested loop, a hash join, and a partitionwise
 * join. Merge joins are further subdivided based on whether the inner side
 * is materialized, and nested loops are further subdivided based on whether
 * the inner side is materialized, memoized, or neither. "Plain" means a
 * strategy where neither materialization nor memoization is used.
 *
 * If you don't care whether materialization or memoization is used, set all
 * the bits for the relevant major join strategy. If you do care, just set the
 * subset of bits that correspond to the cases you want to allow.
 */
#define JSA_FOREIGN						0x0001
#define JSA_MERGEJOIN_PLAIN				0x0002
#define JSA_MERGEJOIN_MATERIALIZE		0x0004
#define JSA_NESTLOOP_PLAIN				0x0008
#define JSA_NESTLOOP_MATERIALIZE		0x0010
#define JSA_NESTLOOP_MEMOIZE			0x0020
#define JSA_HASHJOIN					0x0040
#define JSA_PARTITIONWISE				0x0080

#define JSA_MERGEJOIN_ANY	\
	(JSA_MERGEJOIN_PLAIN | JSA_MERGEJOIN_MATERIALIZE)
#define JSA_NESTLOOP_ANY \
	(JSA_NESTLOOP_PLAIN | JSA_NESTLOOP_MATERIALIZE | JSA_NESTLOOP_MEMOIZE)

/*
 * allpaths.c
 */

extern PGDLLIMPORT bool enable_geqo;
extern PGDLLIMPORT int geqo_threshold;
extern PGDLLIMPORT int min_parallel_table_scan_size;
extern PGDLLIMPORT int min_parallel_index_scan_size;
extern PGDLLIMPORT bool enable_group_by_reordering;

/* Hook for plugins to get control in set_rel_pathlist() */
typedef void (*set_rel_pathlist_hook_type) (PlannerInfo *root,
											RelOptInfo *rel,
											Index rti,
											RangeTblEntry *rte);
extern PGDLLIMPORT set_rel_pathlist_hook_type set_rel_pathlist_hook;

/* Hooks for plugins to get control in add_paths_to_joinrel() */
typedef void (*join_path_setup_hook_type) (PlannerInfo *root,
										   RelOptInfo *joinrel,
										   RelOptInfo *outerrel,
										   RelOptInfo *innerrel,
										   JoinType jointype,
										   JoinPathExtraData *extra);
extern PGDLLIMPORT join_path_setup_hook_type join_path_setup_hook;
typedef void (*set_join_pathlist_hook_type) (PlannerInfo *root,
											 RelOptInfo *joinrel,
											 RelOptInfo *outerrel,
											 RelOptInfo *innerrel,
											 JoinType jointype,
											 JoinPathExtraData *extra);
extern PGDLLIMPORT set_join_pathlist_hook_type set_join_pathlist_hook;

/* Hook for plugins to replace standard_join_search() */
typedef RelOptInfo *(*join_search_hook_type) (PlannerInfo *root,
											  int levels_needed,
											  List *initial_rels,
											  unsigned jsa_mask);
extern PGDLLIMPORT join_search_hook_type join_search_hook;


extern RelOptInfo *make_one_rel(PlannerInfo *root, List *joinlist);
extern RelOptInfo *standard_join_search(PlannerInfo *root, int levels_needed,
										List *initial_rels, unsigned jsa_mask);

extern void generate_gather_paths(PlannerInfo *root, RelOptInfo *rel,
								  bool override_rows);
extern void generate_useful_gather_paths(PlannerInfo *root, RelOptInfo *rel,
										 bool override_rows);
extern int	compute_parallel_worker(RelOptInfo *rel, double heap_pages,
									double index_pages, int max_workers);
extern void create_partial_bitmap_paths(PlannerInfo *root, RelOptInfo *rel,
										Path *bitmapqual);
extern void generate_partitionwise_join_paths(PlannerInfo *root,
											  RelOptInfo *rel);

/*
 * indxpath.c
 *	  routines to generate index paths
 */
extern void create_index_paths(PlannerInfo *root, RelOptInfo *rel);
extern bool relation_has_unique_index_for(PlannerInfo *root, RelOptInfo *rel,
										  List *restrictlist,
										  List *exprlist, List *oprlist);
extern bool indexcol_is_bool_constant_for_query(PlannerInfo *root,
												IndexOptInfo *index,
												int indexcol);
extern bool match_index_to_operand(Node *operand, int indexcol,
								   IndexOptInfo *index);
extern void check_index_predicates(PlannerInfo *root, RelOptInfo *rel);

/*
 * tidpath.c
 *	  routines to generate tid paths
 */
extern bool create_tidscan_paths(PlannerInfo *root, RelOptInfo *rel);

/*
 * joinpath.c
 *	   routines to create join paths
 */
extern void add_paths_to_joinrel(PlannerInfo *root, RelOptInfo *joinrel,
								 RelOptInfo *outerrel, RelOptInfo *innerrel,
								 JoinType jointype, SpecialJoinInfo *sjinfo,
								 List *restrictlist, unsigned jsa_mask);

/*
 * joinrels.c
 *	  routines to determine which relations to join
 */
extern void join_search_one_level(PlannerInfo *root, int level,
								  unsigned jsa_mask);
extern RelOptInfo *make_join_rel(PlannerInfo *root,
								 RelOptInfo *rel1, RelOptInfo *rel2,
								 unsigned jsa_mask);
extern Relids add_outer_joins_to_relids(PlannerInfo *root, Relids input_relids,
										SpecialJoinInfo *sjinfo,
										List **pushed_down_joins);
extern bool have_join_order_restriction(PlannerInfo *root,
										RelOptInfo *rel1, RelOptInfo *rel2);
extern bool have_dangerous_phv(PlannerInfo *root,
							   Relids outer_relids, Relids inner_params);
extern void mark_dummy_rel(RelOptInfo *rel);
extern void init_dummy_sjinfo(SpecialJoinInfo *sjinfo, Relids left_relids,
							  Relids right_relids);

/*
 * equivclass.c
 *	  routines for managing EquivalenceClasses
 */
typedef bool (*ec_matches_callback_type) (PlannerInfo *root,
										  RelOptInfo *rel,
										  EquivalenceClass *ec,
										  EquivalenceMember *em,
										  void *arg);

extern bool process_equivalence(PlannerInfo *root,
								RestrictInfo **p_restrictinfo,
								JoinDomain *jdomain);
extern Expr *canonicalize_ec_expression(Expr *expr,
										Oid req_type, Oid req_collation);
extern void reconsider_outer_join_clauses(PlannerInfo *root);
extern void rebuild_eclass_attr_needed(PlannerInfo *root);
extern EquivalenceClass *get_eclass_for_sort_expr(PlannerInfo *root,
												  Expr *expr,
												  List *opfamilies,
												  Oid opcintype,
												  Oid collation,
												  Index sortref,
												  Relids rel,
												  bool create_it);
extern EquivalenceMember *find_ec_member_matching_expr(EquivalenceClass *ec,
													   Expr *expr,
													   Relids relids);
extern EquivalenceMember *find_computable_ec_member(PlannerInfo *root,
													EquivalenceClass *ec,
													List *exprs,
													Relids relids,
													bool require_parallel_safe);
extern bool relation_can_be_sorted_early(PlannerInfo *root, RelOptInfo *rel,
										 EquivalenceClass *ec,
										 bool require_parallel_safe);
extern void generate_base_implied_equalities(PlannerInfo *root);
extern List *generate_join_implied_equalities(PlannerInfo *root,
											  Relids join_relids,
											  Relids outer_relids,
											  RelOptInfo *inner_rel,
											  SpecialJoinInfo *sjinfo);
extern List *generate_join_implied_equalities_for_ecs(PlannerInfo *root,
													  List *eclasses,
													  Relids join_relids,
													  Relids outer_relids,
													  RelOptInfo *inner_rel);
extern bool exprs_known_equal(PlannerInfo *root, Node *item1, Node *item2,
							  Oid opfamily);
extern EquivalenceClass *match_eclasses_to_foreign_key_col(PlannerInfo *root,
														   ForeignKeyOptInfo *fkinfo,
														   int colno);
extern RestrictInfo *find_derived_clause_for_ec_member(EquivalenceClass *ec,
													   EquivalenceMember *em);
extern void add_child_rel_equivalences(PlannerInfo *root,
									   AppendRelInfo *appinfo,
									   RelOptInfo *parent_rel,
									   RelOptInfo *child_rel);
extern void add_child_join_rel_equivalences(PlannerInfo *root,
											int nappinfos,
											AppendRelInfo **appinfos,
											RelOptInfo *parent_joinrel,
											RelOptInfo *child_joinrel);
extern void add_setop_child_rel_equivalences(PlannerInfo *root,
											 RelOptInfo *child_rel,
											 List *child_tlist,
											 List *setop_pathkeys);
extern List *generate_implied_equalities_for_column(PlannerInfo *root,
													RelOptInfo *rel,
													ec_matches_callback_type callback,
													void *callback_arg,
													Relids prohibited_rels);
extern bool have_relevant_eclass_joinclause(PlannerInfo *root,
											RelOptInfo *rel1, RelOptInfo *rel2);
extern bool has_relevant_eclass_joinclause(PlannerInfo *root,
										   RelOptInfo *rel1);
extern bool eclass_useful_for_merging(PlannerInfo *root,
									  EquivalenceClass *eclass,
									  RelOptInfo *rel);
extern bool is_redundant_derived_clause(RestrictInfo *rinfo, List *clauselist);
extern bool is_redundant_with_indexclauses(RestrictInfo *rinfo,
										   List *indexclauses);

/*
 * pathkeys.c
 *	  utilities for matching and building path keys
 */
typedef enum
{
	PATHKEYS_EQUAL,				/* pathkeys are identical */
	PATHKEYS_BETTER1,			/* pathkey 1 is a superset of pathkey 2 */
	PATHKEYS_BETTER2,			/* vice versa */
	PATHKEYS_DIFFERENT,			/* neither pathkey includes the other */
} PathKeysComparison;

extern PathKeysComparison compare_pathkeys(List *keys1, List *keys2);
extern bool pathkeys_contained_in(List *keys1, List *keys2);
extern bool pathkeys_count_contained_in(List *keys1, List *keys2, int *n_common);
extern List *get_useful_group_keys_orderings(PlannerInfo *root, Path *path);
extern Path *get_cheapest_path_for_pathkeys(List *paths, List *pathkeys,
											Relids required_outer,
											CostSelector cost_criterion,
											bool require_parallel_safe);
extern Path *get_cheapest_fractional_path_for_pathkeys(List *paths,
													   List *pathkeys,
													   Relids required_outer,
													   double fraction);
extern Path *get_cheapest_parallel_safe_total_inner(List *paths);
extern List *build_index_pathkeys(PlannerInfo *root, IndexOptInfo *index,
								  ScanDirection scandir);
extern List *build_partition_pathkeys(PlannerInfo *root, RelOptInfo *partrel,
									  ScanDirection scandir, bool *partialkeys);
extern List *build_expression_pathkey(PlannerInfo *root, Expr *expr,
									  Oid opno,
									  Relids rel, bool create_it);
extern List *convert_subquery_pathkeys(PlannerInfo *root, RelOptInfo *rel,
									   List *subquery_pathkeys,
									   List *subquery_tlist);
extern List *build_join_pathkeys(PlannerInfo *root,
								 RelOptInfo *joinrel,
								 JoinType jointype,
								 List *outer_pathkeys);
extern List *make_pathkeys_for_sortclauses(PlannerInfo *root,
										   List *sortclauses,
										   List *tlist);
extern List *make_pathkeys_for_sortclauses_extended(PlannerInfo *root,
													List **sortclauses,
													List *tlist,
													bool remove_redundant,
													bool remove_group_rtindex,
													bool *sortable,
													bool set_ec_sortref);
extern void initialize_mergeclause_eclasses(PlannerInfo *root,
											RestrictInfo *restrictinfo);
extern void update_mergeclause_eclasses(PlannerInfo *root,
										RestrictInfo *restrictinfo);
extern List *find_mergeclauses_for_outer_pathkeys(PlannerInfo *root,
												  List *pathkeys,
												  List *restrictinfos);
extern List *select_outer_pathkeys_for_merge(PlannerInfo *root,
											 List *mergeclauses,
											 RelOptInfo *joinrel);
extern List *make_inner_pathkeys_for_merge(PlannerInfo *root,
										   List *mergeclauses,
										   List *outer_pathkeys);
extern List *trim_mergeclauses_for_inner_pathkeys(PlannerInfo *root,
												  List *mergeclauses,
												  List *pathkeys);
extern List *truncate_useless_pathkeys(PlannerInfo *root,
									   RelOptInfo *rel,
									   List *pathkeys);
extern bool has_useful_pathkeys(PlannerInfo *root, RelOptInfo *rel);
extern List *append_pathkeys(List *target, List *source);
extern PathKey *make_canonical_pathkey(PlannerInfo *root,
									   EquivalenceClass *eclass, Oid opfamily,
									   int strategy, bool nulls_first);
extern void add_paths_to_append_rel(PlannerInfo *root, RelOptInfo *rel,
									List *live_childrels);

#endif							/* PATHS_H */
