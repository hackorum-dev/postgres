# Star-join planning

This file describes the planner optimization of star joins, controlled
by the `enable_starjoin_ordering` GUC: the overall design, the
conditions for a relation to be treated as a dimension, and the open
questions and issues.

The plan costs, planning memory and timings quoted below were measured
when the corresponding rule was introduced or the issue was found.  The
query corpora are 1500 random star queries, each with some rels without
join clauses (603 of them with a pseudoconstant qual), and three other
corpora of 600 random queries, mixing stars with outer joins, subqueries
and other rels.  One of the three was generated to make the last-ditch
pass of `join_search_one_level` join the dimensions to rels without the
fact.

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
cheaper plan too, e.g. by postponing a selective join.  It can also make
the planner use the standard join search instead of GEQO.

### Parts

The optimization consists of these parts:

- The `enable_starjoin_ordering` GUC (on by default), documented with
  the other planner method configuration parameters.  With the GUC
  disabled, no clusters are built, so the join search is not restricted.
  The dimensions then count fully toward `geqo_threshold`.
- `generate_starjoin_clusters` determines the starjoin clusters of the
  query.  `query_planner` calls it after matching the foreign keys to
  the join clauses, once the lateral, placeholder and special join
  information is available.  It keeps the clusters in the
  `starjoin_clusters` list of `PlannerInfo`, as `StarJoinClusterInfo`
  nodes.  Like other planner-only nodes, `StarJoinClusterInfo` needs no
  copy, equal, read or query jumble support.
- `has_starjoin_fk` looks for a foreign key of the fact that makes a
  relation its dimension, using `has_extra_starjoin_quals` to check that
  the foreign key accounts for all the join clauses between them.
- `starjoin_fk_comparisons_match`, `starjoin_fk_operand_matches` and
  `starjoin_equality_implies` check the operators, input coercions and
  collations of the join clauses.
- `starjoin_order_invalid`, called by `make_join_rel`, rejects joins
  violating the canonical order, just like various other join
  restrictions.
- `starjoin_setup_subproblem`, called by `make_rel_from_joinlist` for
  each join search subproblem, determines the dimensions to check in the
  subproblem.
- `starjoin_geqo_levels` counts the dimensions as fewer rels toward
  `geqo_threshold`.

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

In a subproblem containing the fact of a cluster, joinrels with a
dimension of the cluster but without the fact are cartesian products, as
the dimensions have join clauses only to the fact (and no join order
restrictions).  `join_search_one_level` builds them when the other rel
has no join clauses, or in the last-ditch pass, when no other join is
possible at that level.  They may get joined further, and so the search
might consider many subsets of the dimensions without the fact.  Most of
them are not needed, as the dimensions can be joined to the joinrels
with the fact, in the canonical order.  But joining the other rel to the
fact by a cartesian product multiplies the rows for all the following
joins, while joining it to a dimension (e.g. in the inner of the hash
join of the last dimension to the fact) may be much cheaper.  So a
joinrel with a dimension but without the fact is allowed if it has a
single dimension, and the input without the dimension has no join
clauses, i.e. no equivalence class joins and no joininfo clauses other
than pseudoconstant ones.  This is similar to the test
`join_search_one_level` uses to decide which rels to join by clauseless
joins.  A pseudoconstant qual is in the joininfo list of every rel in
its join domain, but it's not a join clause (it's evaluated just once,
in a gating Result node), so it's ignored.

For example, with a fact table xf (100k rows), dimensions xd1, xd2 and
xd3 (10, 100 and 1000 rows), and tables xo1 and xo2 (1 and 2 rows)
without join clauses:

    SELECT ... FROM xo1, xd3, xf, xd2, xo2, xd1
     WHERE xf.xd2 = xd2.id AND xf.xd3 = xd3.id AND xf.xd1 = xd1.id

With the GUC disabled, the plan hashes the product of xd1, xo1 and xo2
(20 rows) for the last join, so the hash joins process just the 100k
fact rows (cost 9406.66).  If all joinrels with a dimension but without
the fact were rejected, the product of xo1 and xo2 could only be joined
to joinrels with xf, and the cheapest such plan would join it to xf
first, so all three hash joins would process 200k rows (cost 10691.02,
14% more).

Compared to rejecting all these joinrels, allowing them made 400 of the
1500 random star queries cheaper (they were up to 30% more expensive),
and none more expensive.  In the three other corpora, it made 188, 231
and 0 plans cheaper (they were up to 66% and 111% more expensive), and
1, 9 and 0 plans more expensive, by less than 1%, but those 10 plans
cost the same as with the GUC disabled.  The numbers of queries with
plans more expensive than with the GUC disabled (by more than 0.1%)
dropped from 493 to 285, from 199 to 32 and from 235 to 29 (there were
none in the third corpus).  No query failed to plan or tripped an
assertion.  Star queries with a pseudoconstant qual were not affected,
as they have no starjoin clusters (see the open issues).  The search
considers joining the dimensions to the rels without join clauses, which
increased the planning memory with the GUC enabled by 13% for the 897
star queries without a pseudoconstant qual, and by 2.7%, 4.3% and 0% for
the three other corpora (by up to 48% for a single query).  That's still
only 20%, 80%, 34% and 74% of the planning memory with the GUC disabled.

Without the condition on the join clauses (allowing a single dimension
joined to any rel), or without any restriction at all, the plan costs
are the same in all the queries, but the search builds more joinrels.
In 25 queries (23 of them generated for the last-ditch pass), the
planning memory increased by up to 17% (20% without any restriction).

For each set of the other rels, the search builds at most d+1 joinrels
without the fact (with no dimension, or with one of the d dimensions),
just like with the fact.

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
cheapest plan.  Nor can the search forbid joining the dimensions without
the fact in such subproblems, as that may be the only way to join them
(e.g. "d1 CROSS JOIN d2" with `join_collapse_limit` set to 1).  The
product of the dimensions is then joined to the fact in an upper
subproblem, where the dimensions are not initial rels, and so are not
checked.

`starjoin_setup_subproblem` determines the dimensions to check once per
subproblem, and keeps them in the `subproblem_dims` field of each
cluster: the dimensions joined individually in the subproblem if it
contains the fact, or NULL.  `make_rel_from_joinlist` calls it right
after setting the initial rels, before running the join search
(standard, GEQO or `join_search_hook`).  The subproblems are solved one
at a time, and each nested one is solved before the subproblem
containing its result is set up, so the field always describes the
subproblem being searched.  `starjoin_order_invalid` then only
intersects the joinrel with those sets, while walking the initial rels
for every proposed join would dominate the search with many dimensions.
For a star of 100, 200 and 400 dimensions in a single subproblem,
determining the dimensions once per subproblem reduced the median
planning time (release build, with GEQO disabled) from 11.6 to 6.7 ms,
from 68-69 to 24-26 ms, and from 562-568 to 123-124 ms, with the same
plan costs and planning memory (within 1 kB).

### Why the join search cannot fail

The dimensions are interchangeable, as they have join clauses only to
the fact, and no join order restrictions.  So for each pruned joinrel
with the fact, the search builds the joinrel with the dimensions
replaced by the first ones in the canonical order.  A pruned joinrel
without the fact is a cartesian product of the dimensions and other
rels, and the search builds it without the dimensions, which can then be
joined to the joinrels with the fact.  The pruning therefore can't make
the join search fail.

### GEQO

Only the standard join search is restricted, not GEQO.  GEQO evaluates a
fixed number of join orders (tours), each joining all the initial rels,
so the restriction would not make it any cheaper.  It would only add the
overhead of the checks, and make tours with the dimensions in different
orders build the same plan.  For a query joining 5 facts with 10
dimensions each, GEQO planning took 210ms when enforcing the canonical
order and the restrictions on joinrels without the fact, and 151ms
without them.  `starjoin_order_invalid` recognizes the standard join
search by the `join_rel_level` field of `PlannerInfo` being set.

The standard join search joins the dimensions of a cluster to the fact
only in the canonical order, so it considers only d+1 of the 2^d subsets
of the d dimensions.  Joinrels without the fact may contain at most one
of the dimensions, so that's d+1 subsets too.  So `starjoin_geqo_levels`
counts the dimensions as only ceil(log2(d+1)) rels toward
`geqo_threshold`.  The dimensions are those `starjoin_setup_subproblem`
determined for the subproblem, so a cluster has none if its fact is not
part of the subproblem, as the order is not enforced without the fact.
`join_search_hook` gets the number of initial rels without this
discount.

A FROM list of tables is never split into join search subproblems, so
with the default settings a star with 11 or more dimensions would
otherwise be planned by GEQO.  With the discount, it's planned by the
standard join search, which is much cheaper.  A star with 11 dimensions
takes 0.3ms to plan instead of 20ms, and a star with 100 dimensions 6ms,
compared to 300ms with GEQO.  Not counting the dimensions at all would
not be safe, because each cluster still multiplies the number of
joinrels by d+1.  For example, for a query joining 4 facts with 10
dimensions each, the standard join search would take 1.2s and 440MB,
compared to 120ms with GEQO.

## Conditions for a dimension

A relation is a dimension of a fact if it's referenced by a foreign key
of the fact, the join clauses between them fully match the foreign key,
and it satisfies the conditions below.  The conditions are numbered as
in the comment of `generate_starjoin_clusters`.  They make sure each
fact row matches exactly one row of the dimension, and that the
dimension can be joined to the fact at any point of the join search.
The fact has to be a base relation too.

1. The dimension is a plain base relation (`RELOPT_BASEREL`), not an
   ONLY scan of a partitioned table.

   A foreign key referencing a partitioned table is recorded against the
   parent, so a query joining ONLY the parent still matches the foreign
   key.  But a partitioned table stores no rows itself, so the join
   would remove every fact row instead of preserving it.  Joins to the
   whole partitioned table never qualify, as the referenced relation has
   to be scanned without inheritance (see the conditions inherited from
   foreign key matching).  Direct joins to leaf partitions are subject
   to the foreign key requirements below.

2. All join columns on the fact are NOT NULL, and their foreign key
   equality operators are immutable.

   A NULL never matches, so the join would remove a fact row with a NULL
   reference.  The check uses the NOT NULL constraints of the columns
   (`notnullattnums`).  A satisfied foreign key need not have a match
   under a comparison that is not immutable.  For example, a timestamptz
   column referencing a timestamp key is compared by interpreting the
   timestamp in the session's TimeZone, so a fact row inserted at noon
   UTC can lose its match when queried in New York.  Neither type is
   collatable, so the collation checks don't help.  Immutable cross-type
   comparisons are fine.

3. The dimension has neither local restriction quals
   (`baserestrictinfo`) nor a TABLESAMPLE clause.

   A restriction removes some dimension rows, so some fact rows might
   lose their match.  Similarly, a sample of the dimension need not
   contain the referenced row.  A sampled fact is fine, as each
   remaining fact row still has its match.

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
   for by the matched foreign key, with compatible equality semantics
   and no additional equivalence class members other than expressions of
   the fact, in equivalence classes that are not broken.

   A fully matched NOT NULL foreign key does not make the join preserve
   the fact rows if other conditions connect the same pair of relations,
   as they make the join selective.  The canonical order could then
   postpone a selective dimension until all the preceding dimensions are
   joined.  This applies to loose join clauses and to equivalence
   classes of the fact and the dimension, including the equivalence
   class of the foreign key with additional members, as an additional
   equality can be merged into it.

   But a member of the fact alone, as in "f.a = d.id AND f.a = f.b",
   does not make the join selective.  All members of an equivalence
   class are compared with the same operator family and collation, and
   that equality is transitive, so equalities among the members of the
   fact only filter fact rows.  Each remaining fact row matches the same
   dimension row, whichever member a join clause uses.  Constants,
   expressions of several relations, other members of the dimension, and
   members of other relations still disqualify the dimension.

   A broken equivalence class (without a usable equality operator for
   some pair of its members) disqualifies the dimension too.  It applies
   its source clauses as written, instead of equating the members of the
   fact at the fact scan, so a clause comparing the dimension with
   another fact column can reject fact rows in the dimension join.
   Security quals can break the equivalence class of the foreign key:
   once the quals of the query join an equivalence class, only leakproof
   equalities may be derived, so a row-level security policy equating a
   date and a timestamp column of the fact breaks it.

### Foreign key requirements

A fact can reference a dimension through several foreign keys, e.g.
through both a short key and a composite key.  `has_starjoin_fk` checks
the conditions on the relations once, and then tries each matching
foreign key, until one has NOT NULL referencing columns and accounts for
all the join clauses.  So the join on the other column of the composite
key is not mistaken for an extra restriction, when the composite foreign
key accounts for both equalities.  It never combines evidence from
different foreign keys, as their keys need not reference the same
dimension row.

Temporal (PERIOD) foreign keys are skipped, using `conperiod` from the
relcache and the planner foreign key metadata.  Such a foreign key
guarantees only that the union of the matching referenced periods covers
the referencing period, so the join on the overlap operator can return
several rows per fact row, even when the constraint is validated and
satisfied.  They are skipped only when looking for a starjoin foreign
key, not when matching foreign keys for selectivity estimation, and an
ordinary equality foreign key between the same relations remains
eligible.

A foreign key referencing a partitioned table has a pg_constraint row
for each referenced partition, with the OID of that partition and its
unique index.  A join directly to a leaf partition can match such an
entry, although the foreign key guarantees a match only somewhere in the
partition tree, so the unique index of the leaf proves at most one
match, not that there is one.  So a foreign key whose parent constraint
(`conparentid`) references a different relation is rejected, as it's a
referenced-side partition entry.  A constraint inherited on the
referencing side keeps the referenced relation, and is valid evidence,
as is a separately declared foreign key to the leaf.  Foreign keys
without a parent constraint need no extra catalog lookup.

### Equality semantics

Matching foreign keys to join clauses for selectivity estimation does
not check the comparison semantics, which is not enough to prune join
orders.  For example, a case-insensitive join to a text primary key
using the C collation can match both 'a' and 'A' for a single fact row.
A satisfied foreign key guarantees a match under the equality of the
referenced column, and the unique index enforces uniqueness under its
own equality.  `starjoin_fk_comparisons_match` finds the unique index of
the foreign key (`conindid`), and matches its keys to the referenced
columns, not to the foreign key positions (REFERENCES can list the
columns in a different order).  If the index is not available to the
planner (not in the index list of the dimension), the foreign key is not
used.

Two directional implications are checked for each key column.  Values
equal under the collation of the referenced column must be equal under
the collation of the join, so that the join finds the referenced row.
Values equal under the collation of the join must be equal under the
collation of the index, so that the join finds no other row.  Requiring
all three collations to agree on equality would reject joins that are
still single-row lookups, such as a join under "C" to a "C" key with a
case-insensitive unique index.  `starjoin_equality_implies` accepts
collations that agree on equality, and a deterministic first collation,
as equality under a deterministic collation is byte-wise equality, which
implies equality under any collation.  That holds for the built-in
string equalities, which compare strings under a deterministic collation
byte-wise, but the equality operator of a foreign key need not compare
the original strings.  An operator class that lower-cases its inputs,
for example, distinguishes I from dotless i in English and matches them
in Turkish, while English and a nondeterministic Turkish collation
disagree on how to fold I.  So with different collations,
`starjoin_equality_implies` accepts the implication only if both the
operator family of the unique index and the equality function of the
foreign key are built-in string comparisons, and otherwise the
collations have to be the same.  Both are checked, as extensions can add
cross-type operators to a built-in family.  Identical collations need no
such proof.

An equality useful for estimating join selectivity need not preserve
fact rows either.  For example, a varchar foreign key referencing a name
key is checked with "name = name", so converting the reference to name
truncates it, while the join uses "name = text" and compares the full
string.  Both operators are immutable and in the same B-tree family.  So
the equivalence class source clauses involving the dimension, and the
loose quals matched outside equivalence classes, have to use the PK = FK
operator of the foreign key (`conpfeqop`), or its commutator for
reversed operands.  Both matchers look through binary relabels, so
`starjoin_fk_operand_matches` checks that the referential integrity
checks pass each column to that operator using only binary-coercible
casts too.  The operator OID alone is not enough, as a binary cast
through a third type can bypass the implicit cast used by the
referential integrity checks.  For polymorphic inputs, the relabel must
not change the actual type seen by the operator either.  The parser
passes a domain over an array, range or multirange type to a polymorphic
operator such as "anyarray = anyarray" by relabeling it to its base
type, so the base type of such a domain is accepted too.  The
referential integrity checks apply the same relabel, so the operator
gets the same values with the same actual types.  The equivalence class
source clauses not involving the dimension only filter fact rows.  But
such a clause can bring the foreign key column into the equivalence
class, while the clauses involving the dimension name another fact
column, so the members that are the foreign key column are checked the
same way as the operands.  Genuine cross-type operators and
binary-compatible coercions remain eligible, and the collation checks
still apply.  Keys needing a cast that is not binary-coercible (e.g.
integer to numeric) are not used, even if the cast preserves the value.

These checks change only which relations are treated as dimensions, not
foreign key enforcement or selectivity estimation.

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
- A pseudoconstant qual (e.g. "now() > '2000-01-01'", or an uncorrelated
  EXISTS) gets the relids of the whole join domain, so it's in the
  joininfo list of every rel, and every dimension appears to have join
  clauses to rels other than the fact.  So a single such qual disables
  all starjoin clusters in the query, even if it's in the ON clause of a
  single dimension's join.  The dimensions then count fully toward
  `geqo_threshold`, so with the default settings a star with 11
  dimensions gets planned by GEQO.  Ignoring pseudoconstant quals when
  building the clusters would be enough to fix this.  The quals would
  still make the dimensions joinable to each other without the fact, but
  such joinrels are restricted (see `starjoin_order_invalid`).
- A dimension gets joined to the fact at its position in the canonical
  order, so if a rel without join clauses is joined to a dimension
  that's not the last one, the joins of the following dimensions process
  the multiplied rows.  The clauseless_first_cost_higher check in
  starjoin_other shows this, for a star with two scans of a 2-row table
  without join clauses.
- `join_search_hook` gets the number of initial rels without the
  discount, so an extension implementing its own join search has to call
  `starjoin_geqo_levels` to make the same choice between the standard
  join search and GEQO.
- The foreign key suites infer whether a relation qualifies as a
  dimension from the planning memory, accepting up to 10% more than when
  planning the canonical order (see the regression tests).  That's an
  indirect check, and changes of the planning memory use could flip its
  results.
- No regression test makes `starjoin_fk_operand_matches` reject a
  relabel changing the actual type seen by a polymorphic operator.

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
  subproblems, eager aggregation, and rels without join clauses.  Each
  check is a single boolean, so that the output does not depend on
  platform-specific costs or memory usage, and some checks show the
  known limitations.
- starjoin_sample checks that the GUC does not change the plans starting
  with a sample of a dimension estimated to have a single row (which the
  canonical order would join last), and that the join search for a
  sampled fact is still pruned.
- starjoin_geqo checks which star queries are planned by GEQO, and how
  the GUC affects their plans.  It detects GEQO by setting
  `geqo_pool_size` so large that GEQO fails to allocate the population.
- starjoin_geqo_large plans a star with 80 dimensions with GEQO enabled,
  with the GUC enabled and disabled, and with GEQO disabled as a
  control, for `geqo_threshold` set to 2 and 12.  At 12, the fact and
  the dimensions count as 8 rels with the GUC enabled, so the standard
  join search is used.  It compares the plan costs, and reports the
  planning times through the starjoin_geqo_timings psql variable.  It's
  commented out in parallel_schedule, like the other _large suites.
- The foreign key suites check which relations qualify as dimensions.
  If all the dimensions of a star qualify, the join search builds only
  the joinrels of the canonical order, and needs about as much planning
  memory as with `join_collapse_limit` set to 1, which forces that
  order.  A relation that does not qualify can be joined at any other
  point, so the search takes noticeably more memory.  So the suites
  compare the planning memory of the search and of the forced order,
  accepting up to 10% more, with four other dimensions that always
  qualify to widen the difference.  The starjoin_fk_collation and
  starjoin_fk_opclass suites need a UTF8 database with ICU, and have
  alternative outputs for when they're skipped.
  - starjoin_fk_overlap: a composite foreign key qualifies alone and
    together with a short one, whichever is examined first (varying both
    the creation and the name order), while a lone short key and two
    separate single-column foreign keys don't.
  - starjoin_fk_period: a PERIOD foreign key, where each fact row joins
    eight adjacent periods, does not qualify, while an ordinary equality
    foreign key between the same tables does.
  - starjoin_fk_collation: joins under deterministic collations qualify,
    while a case-insensitive join does not, both as an equivalence class
    and as a loose qual.  A nondeterministic collation agreeing with
    both the column and the index qualifies, on a composite key with the
    index columns in a different order.  An index with a collation
    override shows that checking only the column or only the index is
    not enough (one join matches two rows, the other none).  Also a "C"
    key with a case-insensitive unique index, where joins under either
    collation qualify, while an accent-insensitive join can match two
    rows and does not.
  - starjoin_fk_opclass: a case-folding operator class through a loose
    qual and an equivalence class, a mismatch with just the backing
    index, and a nondeterministic index collation that separately breaks
    the existence and the uniqueness proof, where only the native
    comparison qualifies.
  - starjoin_fk_only: an ONLY scan of the referenced partitioned parent
    matches no rows and does not qualify, while an ONLY scan of a
    directly referenced leaf partition does.
  - starjoin_fk_partition: all 100 fact rows join the partitioned
    parent, but only one joins the selected leaf, which does not qualify
    on the foreign key of the parent alone, but does on a genuine
    foreign key to it, both before and after attaching the fact table as
    a referencing partition.
  - starjoin_fk_timezone: a composite foreign key whose second
    comparison is time-zone-dependent, so the join returns all 100 rows
    in UTC and none in New York, and must not qualify in either zone.
    An otherwise identical foreign key using timestamptz on both sides
    still qualifies, including with an immutable int/smallint comparison
    on its first column.
  - starjoin_fk_coercion: the name truncation through an equivalence
    class, and a same-operator cast that bypasses lowercasing, both as a
    loose qual and as an equivalence class (including through another
    fact column).  A commuted name/text comparison, a binary
    varchar-to-text conversion and a fact-only expression in the
    equivalence class still qualify.  So do domains over int[] on the
    referencing side and over int4range on the referenced side.
