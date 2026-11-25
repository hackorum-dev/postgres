# Star-join planning

This file describes the planner optimization of star joins, controlled
by the `enable_starjoin_ordering` GUC: the overall design, the
conditions for a relation to be treated as a dimension, and the open
questions and issues.

## Overall design

### Motivation

In a star join, a fact table references several dimension tables through
foreign keys.  When a dimension is joined to the fact through a foreign
key with NOT NULL referencing columns, and the dimension is not
restricted in any other way, each fact row matches exactly one dimension
row.  All orders of joining such dimensions to the fact produce the same
row counts, yet the standard join search considers all of them, building
a joinrel for every subset of the dimensions joined to the fact.  With d
dimensions, that's 2^d subsets, each combined with every set of the
other rels joined to the fact.  Restricting the join search to a
canonical order of the dimensions makes the number of these joinrels
linear in the number of dimensions, so planning such queries gets much
cheaper.

The restriction never changes the query results, only the join orders
the search considers.  But the orders may still differ in cost (e.g. due
to different widths of the dimensions), so the planner may occasionally
miss a cheaper plan.  A relation treated as a dimension although its
join does not preserve the fact rows can only make the planner miss a
cheaper plan too, e.g. by postponing a selective join.

### Parts

The optimization consists of these parts:

- The `enable_starjoin_ordering` GUC (on by default), documented with
  the other planner method configuration parameters.  With the GUC
  disabled, no clusters are built, so the join search is not restricted.
- `generate_starjoin_clusters` determines the starjoin clusters of the
  query.  `query_planner` calls it after matching the foreign keys to
  the join clauses, once the lateral, placeholder and special join
  information is available.  It keeps the clusters in the
  `starjoin_clusters` list of `PlannerInfo`, as `StarJoinClusterInfo`
  nodes.  Like other planner-only nodes, `StarJoinClusterInfo` needs no
  copy, equal, read or query jumble support.
- `has_extra_starjoin_quals` checks that the foreign key accounts for
  all the join clauses between the fact and a dimension.
- `starjoin_order_invalid`, called by `make_join_rel`, rejects joins
  violating the canonical order, just like various other join
  restrictions.

The clusters are determined in `src/backend/optimizer/plan/initsplan.c`,
and the join order is restricted in
`src/backend/optimizer/path/joinrels.c`.

### Starjoin clusters

A starjoin cluster is a fact (a base relation) with at least two
dimensions.  A dimension is a relation referenced by a foreign key of
the fact, fully matched by the join clauses, and satisfying the
conditions described below.  `generate_starjoin_clusters` groups the
foreign keys matched to the join clauses by the referencing and the
referenced relation, checks the conditions for each relation referenced
by a fact, and builds a cluster for each fact with at least two
qualifying dimensions.  A relation failing the conditions is simply not
a dimension of the cluster, and gets joined like any other rel.

The clusters are disjoint.  A relation referenced by two facts has join
clauses to both, so it's a dimension of neither, and a fact has join
clauses to its dimensions, so it's not a dimension of another cluster.
In a snowflake schema, a dimension with its own dimensions is not a
member of the cluster (it has join clauses to them), but it may be the
fact of another cluster.

### The canonical order

The canonical order of a cluster joins the dimensions in relid order,
which is usually the order in which they appear in the query.

`starjoin_order_invalid` enforces the canonical decomposition of any
joinrel containing the fact: the cluster dimensions in the joinrel must
form a prefix of the canonical order.  This applies even if only one
side of the join has dimensions, so that the fact gets joined to the
dimensions in the canonical order, and not e.g. to the second dimension
first.  A missing dimension is fine on its own, as the joinrel may not
include all the dimensions yet.  It's a violation only if the joinrel
includes a dimension after it, i.e. if the first missing dimension
precedes the last dimension in the joinrel.  Other rels may be joined at
any point.  For each set of the other rels, the search then builds only
d+1 joinrels with the fact (with the first k dimensions, for k from 0 to
d), instead of 2^d.

### Joinrels without the fact

Joins without the fact are not restricted, even in subproblems including
the fact.  The dimensions have join clauses only to the fact (and no
join order restrictions), so `join_search_one_level` joins them to rels
without the fact only by cartesian products, e.g. in the last-ditch
pass, when no other join is possible at that level.  Such joins with
dimensions on both sides are rare, but possible: if the other rels have
join clauses usable only at a later level, two such joinrels (each with
a different dimension) may get joined.  The canonical order is not
enforced for them, for the same reason as in subproblems without the
fact (see below).

### Join search subproblems

The clusters are determined for the whole query, but the join search may
be split into multiple subproblems (`join_collapse_limit`,
`from_collapse_limit`, FULL JOIN), each solved separately.  The
restriction applies only to subproblems including the fact, and only to
dimensions that are initial rels of the subproblem (base relations
joined individually).  Dimensions joined into a joinrel by an earlier
subproblem (possibly together with the fact) are treated like any other
rel.  Otherwise the restriction might leave no valid join order, and
planning would fail with "failed to build any N-way joins".

The canonical order must not be enforced without the fact.  It relies on
joins of dimensions to the fact not changing the cardinality of the
fact.  Without the fact, the dimensions can only be joined by cartesian
products (they have join clauses only to the fact), which do change the
cardinality, so the order matters, and enforcing it might eliminate the
cheapest plan.  The product of the dimensions is then joined to the fact
in an upper subproblem, where the dimensions are not initial rels, and
so are not checked.

`starjoin_order_invalid` walks the initial rels of the current
subproblem on each call, to find the base rels of the subproblem, and
whether it contains the fact of each cluster.

### Why the join search cannot fail

The dimensions are interchangeable, as they have join clauses only to
the fact, and no join order restrictions.  So for each pruned joinrel
with the fact, the search builds the joinrel with the dimensions
replaced by the first ones in the canonical order.  The pruning
therefore can't make the join search fail.

### GEQO

GEQO is subject to the same restriction, as it builds the joinrels using
`make_join_rel` too.  So it joins the dimensions in the canonical order,
whatever their order in the tour.  It joins a dimension only to the
clump with the fact (the only desirable join), and a dimension that
can't be joined yet remains a separate clump.  GEQO tries to merge it
again whenever the clump with the fact grows, and at the end it
force-joins the remaining clumps, so it still finds a plan.

## Conditions for a dimension

A relation is a dimension of a fact if it's referenced by a foreign key
of the fact, the join clauses between them fully match the foreign key,
and it satisfies the conditions below.  The conditions are numbered as
in the comment of `generate_starjoin_clusters`.  They make sure each
fact row matches exactly one row of the dimension, and that the
dimension can be joined to the fact at any point of the join search.
The fact has to be a base relation too.

1. The dimension is a plain base relation (`RELOPT_BASEREL`).

2. All join columns on the fact are NOT NULL.

   A NULL never matches, so the join would remove a fact row with a NULL
   reference.  The check uses the NOT NULL constraints of the columns
   (`notnullattnums`).

3. The dimension has no local restriction quals (`baserestrictinfo`).

   A restriction removes some dimension rows, so some fact rows might
   lose their match.

4. The dimension has no relevant join clauses to any rel other than the
   fact (checked via `have_relevant_joinclause`, which also covers
   equivalence class joins).

   So the dimension can be joined only to the fact (or to joinrels
   including it), which makes the dimensions interchangeable.

5. The dimension is not in the minimal LHS or RHS of any
   `SpecialJoinInfo` (it may be in the syntactic LHS, e.g. of a LEFT
   JOIN written after it).

   Only the minimal sets restrict the join order.  A dimension in the
   syntactic RHS of a special join is always in a minimal set of some
   special join, so it's still rejected.

6. The dimension has no lateral references in or out.

7. The dimension is not constrained by any `PlaceHolderInfo`.

   Conditions 5 to 7 rule out join order restrictions on the dimension.

8. All join conditions between the fact and the dimension are accounted
   for by the matched foreign key, with no additional equivalence class
   members.

   A fully matched NOT NULL foreign key does not make the join preserve
   the fact rows if other conditions connect the same pair of relations,
   as they make the join selective.  The canonical order could then
   postpone a selective dimension until all the preceding dimensions are
   joined.  This applies to loose join clauses and to equivalence
   classes of the fact and the dimension, including the equivalence
   class of the foreign key with additional members, as an additional
   equality can be merged into it.

### Foreign key requirements

If the fact has several foreign keys referencing the dimension, the
conditions are checked for the first one in `fkey_list`.

### Conditions inherited from foreign key matching

The clusters are built from the foreign keys the planner matches to the
join clauses for selectivity estimation (`fkey_list`), so they inherit
its conditions:

- Foreign keys that are not enforced are ignored, while NOT VALID ones
  are used (see the open issues).
- The foreign keys of an inheritance parent are ignored, so it can't be
  a fact.
- The referenced range table entry has to be a plain relation without
  inheritance, so an inheritance parent can't be a dimension.
- Self-referential foreign keys are ignored.
- Only foreign keys between two base relations, fully matched by the
  join clauses, are kept.

## Open questions and issues

None of these can make a query return wrong results.  They affect only
the plans, the planning cost, or the regression tests.

- The restriction could be made even stricter, if needed, e.g. to
  effectively join all the dimensions at once, with no other joins in
  between.
- A dimension can't be joined by an outer join (condition 5), although a
  LEFT JOIN to a dimension preserves the fact rows too, as long as the
  dimension has a unique constraint on the join columns.  The condition
  could be relaxed to allow such left joins.
- The canonical order is the relid order, as the cluster keeps its
  dimensions in a bitmap of relids.  Allowing snowflake joins (a
  dimension with its own dimensions as a member of the cluster) would
  need an explicit, ordered list of the dimensions.
- NOT VALID foreign keys are trusted, as in selectivity estimation,
  although some referencing rows need not have a match.  That can affect
  only the plan quality.
- Even if all the joins preserve the cardinality, the join orders don't
  have the same cost.  With a small `work_mem`, the hash join to a large
  dimension needs multiple batches, and the cost of batching depends on
  the width of the outer rows, so it's much cheaper to join the large
  dimension before the wide ones.  If the canonical order joins it last,
  the planner misses the cheaper plan.  This is shown by starjoin_width
  and starjoin_width_large, and by the width_cost_higher check in
  starjoin_other.  There's no fix, as this comes with considering a
  single order.
- With GROUP BY on a column of a dimension, eager aggregation can place
  a partial aggregate on the join of the fact and the other dimensions,
  and then join the grouping dimension.  With the canonical order, that
  joinrel is built only if the grouping dimension is the last one, so
  the plan depends on the order of the dimensions in the FROM clause
  (see starjoin_eager, and the eager_first_partial_agg_on check in
  starjoin_other).  A possible direction, not tried yet, is an explicit
  canonical order putting the dimensions with grouping columns last.
- The search might forbid additional joins, e.g. joinrels containing a
  dimension of a cluster but not its fact.

## Regression tests

- join.sql checks that a simple star join gets the same plan with the
  GUC enabled and disabled.
- starjoin_planning checks that planning succeeds when the join search
  is split into subproblems.
- starjoin_reproducers checks queries where the clusters (determined for
  the whole query) and the join search subproblems don't align, so the
  canonical order can't be followed in some subproblem, and a query
  where the last-ditch pass joins dimensions to rels without the fact.
  For each query, it shows the plan with the GUC enabled, and checks
  that the query returns the same result with the GUC enabled and
  disabled.
- starjoin_eager, starjoin_plan and starjoin_width compare the plans
  with the GUC enabled and disabled, for star joins combined with eager
  aggregation, with extra join clauses between the fact and a dimension,
  and with dimensions of very different widths.  They normalize the
  EXPLAIN ANALYZE output, to ignore platform-dependent memory usage and
  hash table sizing, while retaining the actual row counts and the
  distinction between single-batch and multi-batch hash joins.  With
  `work_mem` set to 128kB in starjoin_width, the hash join to its
  largest dimension needs multiple batches, and the plan with the GUC
  enabled is more expensive.
- The _large variants of these three suites use larger tables, and
  starjoin_eager_large and starjoin_width_large set `work_mem` to 400kB,
  so that their hash joins need multiple batches.  They're too expensive
  to run by default, so they're commented out in parallel_schedule.
- starjoin_other checks various other cases, mostly by comparing the
  plan costs or the planning memory (reported by EXPLAIN (MEMORY)) with
  the GUC enabled and disabled.  The cases include extra join clauses
  between the fact and a dimension, OR clauses, dimensions of very
  different widths, outer joins and semijoins, stars split into
  subproblems, GEQO, and eager aggregation.  Each check is a single
  boolean, so that the output does not depend on platform-specific costs
  or memory usage, and some checks show the known limitations.
