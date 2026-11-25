--
-- Checks for various other cases with enable_starjoin_ordering (the
-- planning failures are covered by starjoin_reproducers, and GEQO by
-- starjoin_geqo).
--
-- Each check is a single boolean, so that the output does not depend on
-- platform-specific costs or memory usage.  Some checks show known
-- limitations, where the plan with the GUC enabled is worse.
--
-- Some checks are about the planning effort (the number of join rels the
-- join search builds), which is not visible in SQL.  Those checks compare the
-- planner memory usage reported by EXPLAIN (MEMORY), which is proportional
-- to the number of join rels and paths.
--

SET max_parallel_workers_per_gather = 0;

-- total cost of the plan, with enable_starjoin_ordering enabled/disabled
CREATE FUNCTION starjoin_other_cost(query text, starjoin bool) RETURNS float8
LANGUAGE plpgsql AS $$
DECLARE
  plan json;
BEGIN
  PERFORM set_config('enable_starjoin_ordering', starjoin::text, true);
  EXECUTE 'EXPLAIN (FORMAT JSON) ' || query INTO plan;
  RETURN (plan->0->'Plan'->>'Total Cost')::float8;
END;
$$;

-- memory used by the planner (in kB), with enable_starjoin_ordering
-- enabled/disabled
CREATE FUNCTION starjoin_other_memory(query text, starjoin bool) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
  plan json;
BEGIN
  PERFORM set_config('enable_starjoin_ordering', starjoin::text, true);
  EXECUTE 'EXPLAIN (MEMORY, FORMAT JSON) ' || query INTO plan;
  RETURN (plan->0->'Planning'->>'Memory Used')::bigint;
END;
$$;

-- the plan (without costs), with enable_starjoin_ordering enabled/disabled
CREATE FUNCTION starjoin_other_plan(query text, starjoin bool) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
  plan json;
BEGIN
  PERFORM set_config('enable_starjoin_ordering', starjoin::text, true);
  EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || query INTO plan;
  RETURN plan::text;
END;
$$;

-- star with 3 dimensions (same as in starjoin_plan)
CREATE TABLE starjoin_other_p1 (id int PRIMARY KEY, v int, w int) WITH (autovacuum_enabled = off);
CREATE TABLE starjoin_other_p2 (id int PRIMARY KEY, v int, w int) WITH (autovacuum_enabled = off);
CREATE TABLE starjoin_other_p3 (id int PRIMARY KEY, v int, w int) WITH (autovacuum_enabled = off);

INSERT INTO starjoin_other_p1 SELECT i, i % 100, i FROM generate_series(1, 1000) i;
INSERT INTO starjoin_other_p2 SELECT i, i % 100, i FROM generate_series(1, 1000) i;
INSERT INTO starjoin_other_p3 SELECT i, i % 100, i FROM generate_series(1, 1000) i;

CREATE TABLE starjoin_other_pf (id int PRIMARY KEY, d1 int NOT NULL,
  d2 int NOT NULL, d3 int NOT NULL, x int) WITH (autovacuum_enabled = off);

INSERT INTO starjoin_other_pf
SELECT i, 1 + (i * 7) % 1000, 1 + (i * 13) % 1000, 1 + (i * 17) % 1000, i % 1000
  FROM generate_series(1, 20000) i;

-- add the foreign keys after loading the data (faster than checking each row)
ALTER TABLE starjoin_other_pf
  ADD FOREIGN KEY (d1) REFERENCES starjoin_other_p1,
  ADD FOREIGN KEY (d2) REFERENCES starjoin_other_p2,
  ADD FOREIGN KEY (d3) REFERENCES starjoin_other_p3;

-- star with two wide dimensions and a large narrow one (same as in
-- starjoin_width)
CREATE TABLE starjoin_other_wa (id int PRIMARY KEY, t text) WITH (autovacuum_enabled = off);
CREATE TABLE starjoin_other_wb (id int PRIMARY KEY, t text) WITH (autovacuum_enabled = off);
CREATE TABLE starjoin_other_wc (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off);

INSERT INTO starjoin_other_wa SELECT i, repeat(md5(i::text), 16) FROM generate_series(1, 100) i;
INSERT INTO starjoin_other_wb SELECT i, repeat(md5(i::text), 16) FROM generate_series(1, 100) i;
INSERT INTO starjoin_other_wc SELECT i, i FROM generate_series(1, 10000) i;

CREATE TABLE starjoin_other_wf (id int, a int NOT NULL, b int NOT NULL,
  c int NOT NULL) WITH (autovacuum_enabled = off);

INSERT INTO starjoin_other_wf
SELECT i, 1 + i % 100, 1 + (i * 7) % 100, 1 + (i * 13) % 10000
  FROM generate_series(1, 10000) i;

ALTER TABLE starjoin_other_wf
  ADD FOREIGN KEY (a) REFERENCES starjoin_other_wa,
  ADD FOREIGN KEY (b) REFERENCES starjoin_other_wb,
  ADD FOREIGN KEY (c) REFERENCES starjoin_other_wc;

-- star with a small dimension used in GROUP BY (same as in starjoin_eager)
CREATE TABLE starjoin_other_h1 (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off);
CREATE TABLE starjoin_other_h2 (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off);
CREATE TABLE starjoin_other_h3 (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off);

INSERT INTO starjoin_other_h1 SELECT i, i % 10 FROM generate_series(1, 100) i;
INSERT INTO starjoin_other_h2 SELECT i, i FROM generate_series(1, 10000) i;
INSERT INTO starjoin_other_h3 SELECT i, i FROM generate_series(1, 10000) i;

CREATE TABLE starjoin_other_hf (id int, d1 int NOT NULL, d2 int NOT NULL,
  d3 int NOT NULL, x int) WITH (autovacuum_enabled = off);

INSERT INTO starjoin_other_hf
SELECT i, 1 + i % 100, 1 + (i * 7) % 10000, 1 + (i * 13) % 10000, i % 1000
  FROM generate_series(1, 20000) i;

ALTER TABLE starjoin_other_hf
  ADD FOREIGN KEY (d1) REFERENCES starjoin_other_h1,
  ADD FOREIGN KEY (d2) REFERENCES starjoin_other_h2,
  ADD FOREIGN KEY (d3) REFERENCES starjoin_other_h3;

-- star with 10 dimensions d1-d10, a table "a" referenced by the fact, and 8
-- tables c1-c8 joined on a non-unique column (all in the same equivalence
-- class, so each pair of them can be joined)
CREATE TABLE starjoin_other_a (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off);
INSERT INTO starjoin_other_a SELECT i, i FROM generate_series(1, 100) i;

DO $$
BEGIN
  FOR i IN 1..10 LOOP
    EXECUTE format('CREATE TABLE starjoin_other_d%s (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off)', i);
    EXECUTE format('INSERT INTO starjoin_other_d%s SELECT i, i FROM generate_series(1, 100) i', i);
  END LOOP;
  FOR i IN 1..8 LOOP
    EXECUTE format('CREATE TABLE starjoin_other_c%s (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off)', i);
    EXECUTE format('INSERT INTO starjoin_other_c%s SELECT i, i %% 50 FROM generate_series(1, 100) i', i);
  END LOOP;
END;
$$;

CREATE TABLE starjoin_other_f (id int PRIMARY KEY, a int NOT NULL,
  d1 int NOT NULL, d2 int NOT NULL, d3 int NOT NULL, d4 int NOT NULL,
  d5 int NOT NULL, d6 int NOT NULL, d7 int NOT NULL, d8 int NOT NULL,
  d9 int NOT NULL, d10 int NOT NULL) WITH (autovacuum_enabled = off);

INSERT INTO starjoin_other_f
SELECT i, 1 + i % 100, 1 + (i * 3) % 100, 1 + (i * 7) % 100, 1 + (i * 11) % 100,
       1 + (i * 13) % 100, 1 + (i * 17) % 100, 1 + (i * 19) % 100,
       1 + (i * 23) % 100, 1 + (i * 29) % 100, 1 + (i * 31) % 100,
       1 + (i * 37) % 100
  FROM generate_series(1, 1000) i;

ALTER TABLE starjoin_other_f
  ADD FOREIGN KEY (a) REFERENCES starjoin_other_a,
  ADD FOREIGN KEY (d1) REFERENCES starjoin_other_d1,
  ADD FOREIGN KEY (d2) REFERENCES starjoin_other_d2,
  ADD FOREIGN KEY (d3) REFERENCES starjoin_other_d3,
  ADD FOREIGN KEY (d4) REFERENCES starjoin_other_d4,
  ADD FOREIGN KEY (d5) REFERENCES starjoin_other_d5,
  ADD FOREIGN KEY (d6) REFERENCES starjoin_other_d6,
  ADD FOREIGN KEY (d7) REFERENCES starjoin_other_d7,
  ADD FOREIGN KEY (d8) REFERENCES starjoin_other_d8,
  ADD FOREIGN KEY (d9) REFERENCES starjoin_other_d9,
  ADD FOREIGN KEY (d10) REFERENCES starjoin_other_d10;

-- small table, joined to a star without join clauses
CREATE TABLE starjoin_other_s (id int) WITH (autovacuum_enabled = off);
INSERT INTO starjoin_other_s VALUES (1), (2);

VACUUM ANALYZE starjoin_other_p1, starjoin_other_p2, starjoin_other_p3,
  starjoin_other_pf, starjoin_other_wa, starjoin_other_wb, starjoin_other_wc,
  starjoin_other_wf, starjoin_other_h1, starjoin_other_h2, starjoin_other_h3,
  starjoin_other_hf, starjoin_other_a, starjoin_other_d1, starjoin_other_d2,
  starjoin_other_d3, starjoin_other_d4, starjoin_other_d5, starjoin_other_d6,
  starjoin_other_d7, starjoin_other_d8, starjoin_other_d9, starjoin_other_d10,
  starjoin_other_c1, starjoin_other_c2, starjoin_other_c3, starjoin_other_c4,
  starjoin_other_c5, starjoin_other_c6, starjoin_other_c7, starjoin_other_c8,
  starjoin_other_f, starjoin_other_s;

--
-- Extra (non-FK) join clause between the fact and a dimension
--
-- An additional join clause between the fact and a dimension can make the
-- join selective.  Exclude that dimension from the cluster, including when
-- the clause is represented by an EC rather than joininfo.
--
SELECT $$
SELECT count(*) FROM starjoin_other_pf AS pf
  JOIN starjoin_other_p1 AS p1 ON pf.d1 = p1.id
  JOIN starjoin_other_p2 AS p2 ON pf.d2 = p2.id
  JOIN starjoin_other_p3 AS p3 ON pf.d3 = p3.id AND pf.x = p3.w
$$ AS q_extra_clause \gset

SELECT starjoin_other_cost(:'q_extra_clause', true)
       <= starjoin_other_cost(:'q_extra_clause', false) * 1.01 AS extra_clause_cost_ok;

-- A non-equality join qual stays in joininfo.
SELECT $$
SELECT count(*) FROM starjoin_other_pf AS pf
  JOIN starjoin_other_p1 AS p1 ON pf.d1 = p1.id
  JOIN starjoin_other_p2 AS p2 ON pf.d2 = p2.id
  JOIN starjoin_other_p3 AS p3 ON pf.d3 = p3.id AND pf.x > p3.w + 1000
$$ AS q_extra_inequality \gset

SELECT starjoin_other_plan(:'q_extra_inequality', true)
       = starjoin_other_plan(:'q_extra_inequality', false) AS extra_inequality_plan_ok;

-- An extra equality can merge into the same EC that matches the FK.
SELECT $$
SELECT count(*) FROM starjoin_other_pf AS pf
  JOIN starjoin_other_p1 AS p1 ON pf.d1 = p1.id
  JOIN starjoin_other_p2 AS p2 ON pf.d2 = p2.id
  JOIN starjoin_other_p3 AS p3 ON pf.d3 = p3.id AND pf.x + p3.w = p3.id
$$ AS q_extra_ec_member \gset

SELECT starjoin_other_plan(:'q_extra_ec_member', true)
       = starjoin_other_plan(:'q_extra_ec_member', false) AS extra_ec_member_plan_ok;

-- EC members of the fact alone only filter fact rows.  The dimension still
-- qualifies, so the search uses about as much memory as the canonical order.
SELECT $$
SELECT count(*) FROM starjoin_other_pf AS pf
  JOIN starjoin_other_p1 AS p1 ON pf.d1 = p1.id
  JOIN starjoin_other_p2 AS p2 ON pf.d2 = p2.id
  JOIN starjoin_other_p3 AS p3 ON pf.d3 = p3.id AND pf.d3 = pf.x
$$ AS q_fact_ec_member \gset

SET join_collapse_limit = 1;
SELECT starjoin_other_memory(:'q_fact_ec_member', true) AS memory_fact_ec_member \gset
RESET join_collapse_limit;
SELECT starjoin_other_memory(:'q_fact_ec_member', true)
       <= :memory_fact_ec_member * 1.1 AS fact_ec_member_canonical;

-- A broken EC applies its source clauses as written, instead of equating the
-- fact's members at the fact scan, so here bf.x = bk.k filters in the
-- dimension join.  Security quals can break the EC: the policy's date =
-- timestamp equality isn't leakproof, and once the query's quals join the
-- EC, it may derive only leakproof equalities.  The dimension must not
-- qualify, so that the selective join can go first.
CREATE ROLE regress_starjoin_other;
SET ROLE regress_starjoin_other;
CREATE TEMP TABLE starjoin_other_bd (id int PRIMARY KEY);
CREATE TEMP TABLE starjoin_other_bk (k date PRIMARY KEY);
CREATE TEMP TABLE starjoin_other_bf (
  d1 int NOT NULL REFERENCES starjoin_other_bd,
  d2 int NOT NULL REFERENCES starjoin_other_bd,
  d3 int NOT NULL REFERENCES starjoin_other_bd,
  k date NOT NULL REFERENCES starjoin_other_bk,
  ts timestamp NOT NULL, x date NOT NULL);
INSERT INTO starjoin_other_bd SELECT i FROM generate_series(1, 1000) i;
INSERT INTO starjoin_other_bk
SELECT date '2000-01-01' + i FROM generate_series(1, 1000) i;
INSERT INTO starjoin_other_bf
SELECT 1 + i % 1000, 1 + (i * 7) % 1000, 1 + (i * 13) % 1000,
       date '2000-01-01' + 1 + i % 1000, date '2000-01-01' + 1 + i % 1000,
       date '2000-01-01' + i % 5000
  FROM generate_series(1, 10000) i;
ANALYZE starjoin_other_bd, starjoin_other_bk, starjoin_other_bf;
ALTER TABLE starjoin_other_bf ENABLE ROW LEVEL SECURITY;
ALTER TABLE starjoin_other_bf FORCE ROW LEVEL SECURITY;
CREATE POLICY starjoin_other_bf_ts ON starjoin_other_bf USING (k = ts);

SELECT $$
SELECT count(*) FROM starjoin_other_bf AS bf
  JOIN starjoin_other_bd AS d1 ON bf.d1 = d1.id
  JOIN starjoin_other_bd AS d2 ON bf.d2 = d2.id
  JOIN starjoin_other_bd AS d3 ON bf.d3 = d3.id
  JOIN starjoin_other_bk AS bk ON bf.k = bk.k AND bf.x = bk.k
$$ AS q_broken_ec \gset

SELECT starjoin_other_cost(:'q_broken_ec', true)
       <= starjoin_other_cost(:'q_broken_ec', false) * 1.01 AS broken_ec_cost_ok;

RESET ROLE;
DROP TABLE starjoin_other_bf, starjoin_other_bk, starjoin_other_bd;
DROP ROLE regress_starjoin_other;

--
-- OR clause, from which extract_restriction_or_clauses() derives a
-- restriction on the dimension, which excludes the dimension from the
-- cluster (the clusters are built after extracting the restrictions)
--
SELECT $$
SELECT count(*) FROM starjoin_other_pf AS pf
  JOIN starjoin_other_p1 AS p1 ON pf.d1 = p1.id
  JOIN starjoin_other_p2 AS p2 ON pf.d2 = p2.id
  JOIN starjoin_other_p3 AS p3 ON pf.d3 = p3.id
 WHERE (p3.w = 1 AND pf.x = 1) OR (p3.w = 2 AND pf.x = 2)
$$ AS q_or_clause \gset

SELECT starjoin_other_cost(:'q_or_clause', true)
       <= starjoin_other_cost(:'q_or_clause', false) * 1.01 AS or_clause_cost_ok;

--
-- Pure FK joins, with dimensions of very different widths
--
-- Even if all joins preserve the cardinality, the join orders don't have the
-- same cost.  With a small work_mem the hash join to the large dimension (wc)
-- needs multiple batches, and the cost of batching depends on the width of
-- the outer rows.  So it's much cheaper to join wc before the two wide
-- dimensions, but wc is the last one in the canonical order.
--
SELECT $$
SELECT wa.t, wb.t, wc.v FROM starjoin_other_wf AS wf
  JOIN starjoin_other_wa AS wa ON wf.a = wa.id
  JOIN starjoin_other_wb AS wb ON wf.b = wb.id
  JOIN starjoin_other_wc AS wc ON wf.c = wc.id
$$ AS q_width \gset

-- Use 128kB, so that the hash join to wc needs multiple batches on all
-- platforms.  With 256kB that's barely the case on 64-bit platforms (the
-- hash table fits with work_mem of about 268kB), and not at all on 32-bit
-- platforms, where the hash tuples are smaller and the hash table fits with
-- less than 200kB.
SET work_mem = '128kB';
-- the canonical order joins wc last, so the plan with the GUC enabled is
-- more expensive
SELECT starjoin_other_cost(:'q_width', true)
       > starjoin_other_cost(:'q_width', false) * 1.01 AS width_cost_higher;
RESET work_mem;

--
-- Canonical pruning
--
-- Joins of the fact are checked even if only one side has dimensions, so
-- for the star with 10 dimensions the join search builds only the 10 join
-- rels in the canonical order.  Compare to planning the canonical order
-- directly, forced by join_collapse_limit = 1 (the plans are the same).
--
SELECT $$
SELECT count(*) FROM starjoin_other_f AS f
  JOIN starjoin_other_d1 AS d1 ON f.d1 = d1.id
  JOIN starjoin_other_d2 AS d2 ON f.d2 = d2.id
  JOIN starjoin_other_d3 AS d3 ON f.d3 = d3.id
  JOIN starjoin_other_d4 AS d4 ON f.d4 = d4.id
  JOIN starjoin_other_d5 AS d5 ON f.d5 = d5.id
  JOIN starjoin_other_d6 AS d6 ON f.d6 = d6.id
  JOIN starjoin_other_d7 AS d7 ON f.d7 = d7.id
  JOIN starjoin_other_d8 AS d8 ON f.d8 = d8.id
  JOIN starjoin_other_d9 AS d9 ON f.d9 = d9.id
  JOIN starjoin_other_d10 AS d10 ON f.d10 = d10.id
$$ AS q_star \gset

SET join_collapse_limit = 1;
SELECT starjoin_other_plan(:'q_star', true) AS plan_canonical,
       starjoin_other_memory(:'q_star', true) AS memory_canonical \gset

-- a single subproblem with 11 rels (below geqo_threshold)
SET join_collapse_limit = 20;
SELECT starjoin_other_plan(:'q_star', true) = :'plan_canonical' AS flat_plan_canonical;
-- the search uses about as much memory as planning the canonical order
SELECT starjoin_other_memory(:'q_star', true) <= :memory_canonical * 1.1 AS flat_search_canonical;
RESET join_collapse_limit;

--
-- Outer joins and semijoins
--
-- Dimensions must not be constrained by any SpecialJoinInfo, but only the
-- minimal relids (min_lefthand/min_righthand) matter.  So a LEFT JOIN or a
-- semijoin (EXISTS) referencing only the fact does not disable the
-- optimization, even if written after the star joins, with the dimensions in
-- its syntactic LHS.  Each query has 8 rels (a single subproblem), and with
-- the optimization the join search needs much less memory.
--
SELECT $$
SELECT count(a.v) FROM starjoin_other_f AS f
  LEFT JOIN starjoin_other_a AS a ON f.a = a.id
  JOIN starjoin_other_d1 AS d1 ON f.d1 = d1.id
  JOIN starjoin_other_d2 AS d2 ON f.d2 = d2.id
  JOIN starjoin_other_d3 AS d3 ON f.d3 = d3.id
  JOIN starjoin_other_d4 AS d4 ON f.d4 = d4.id
  JOIN starjoin_other_d5 AS d5 ON f.d5 = d5.id
  JOIN starjoin_other_d6 AS d6 ON f.d6 = d6.id
$$ AS q_left_join_first \gset

SELECT $$
SELECT count(a.v) FROM starjoin_other_f AS f
  JOIN starjoin_other_d1 AS d1 ON f.d1 = d1.id
  JOIN starjoin_other_d2 AS d2 ON f.d2 = d2.id
  JOIN starjoin_other_d3 AS d3 ON f.d3 = d3.id
  JOIN starjoin_other_d4 AS d4 ON f.d4 = d4.id
  JOIN starjoin_other_d5 AS d5 ON f.d5 = d5.id
  JOIN starjoin_other_d6 AS d6 ON f.d6 = d6.id
  LEFT JOIN starjoin_other_a AS a ON f.a = a.id
$$ AS q_left_join_last \gset

SELECT $$
SELECT count(*) FROM starjoin_other_f AS f
  JOIN starjoin_other_d1 AS d1 ON f.d1 = d1.id
  JOIN starjoin_other_d2 AS d2 ON f.d2 = d2.id
  JOIN starjoin_other_d3 AS d3 ON f.d3 = d3.id
  JOIN starjoin_other_d4 AS d4 ON f.d4 = d4.id
  JOIN starjoin_other_d5 AS d5 ON f.d5 = d5.id
  JOIN starjoin_other_d6 AS d6 ON f.d6 = d6.id
 WHERE EXISTS (SELECT 1 FROM starjoin_other_a AS a WHERE a.v = f.a)
$$ AS q_exists \gset

SELECT starjoin_other_memory(:'q_left_join_first', true)
       < starjoin_other_memory(:'q_left_join_first', false) * 0.5 AS left_join_first_pruned;
-- pruning with the LEFT JOIN at the end
SELECT starjoin_other_memory(:'q_left_join_last', true)
       < starjoin_other_memory(:'q_left_join_last', false) * 0.5 AS left_join_last_pruned;
-- pruning with a semijoin
SELECT starjoin_other_memory(:'q_exists', true)
       < starjoin_other_memory(:'q_exists', false) * 0.5 AS exists_pruned;

--
-- Star split into subproblems
--
-- With the default join_collapse_limit, the star with 10 dimensions is split
-- into two subproblems, and the canonical order is enforced in both.  The
-- checks with GEQO (and with the whole star in a single subproblem above
-- geqo_threshold) are in starjoin_geqo.
--
SELECT starjoin_other_memory(:'q_star', true)
       < starjoin_other_memory(:'q_star', false) * 0.5 AS split_search_pruned;

--
-- Overhead in subproblems unrelated to the star
--
-- If the query has a cluster, the canonical order is checked for each join in
-- all subproblems, including a large separate subproblem (8 rels joinable in
-- any order).  With only two dimensions there's little to prune in the star,
-- so the planning should not use more memory with the GUC enabled (e.g. if
-- the check leaked memory).
--
SELECT $$
SELECT count(*)
  FROM (starjoin_other_f AS f
        JOIN starjoin_other_d1 AS d1 ON f.d1 = d1.id
        JOIN starjoin_other_d2 AS d2 ON f.d2 = d2.id)
  JOIN (starjoin_other_c1 AS c1
        JOIN starjoin_other_c2 AS c2 ON c1.v = c2.v
        JOIN starjoin_other_c3 AS c3 ON c2.v = c3.v
        JOIN starjoin_other_c4 AS c4 ON c3.v = c4.v
        JOIN starjoin_other_c5 AS c5 ON c4.v = c5.v
        JOIN starjoin_other_c6 AS c6 ON c5.v = c6.v
        JOIN starjoin_other_c7 AS c7 ON c6.v = c7.v
        JOIN starjoin_other_c8 AS c8 ON c7.v = c8.v) ON f.id = c1.id
$$ AS q_subproblems \gset

SELECT starjoin_other_memory(:'q_subproblems', true)
       <= starjoin_other_memory(:'q_subproblems', false) * 1.01 AS subproblems_memory_ok;

-- with the default join_collapse_limit, c1 and c2 are a separate subproblem,
-- searched before the star, which is still pruned
SELECT $$
SELECT count(*)
  FROM (starjoin_other_c1 AS c1
        JOIN starjoin_other_c2 AS c2 ON c1.v = c2.v)
  JOIN (starjoin_other_f AS f
        JOIN starjoin_other_d1 AS d1 ON f.d1 = d1.id
        JOIN starjoin_other_d2 AS d2 ON f.d2 = d2.id
        JOIN starjoin_other_d3 AS d3 ON f.d3 = d3.id
        JOIN starjoin_other_d4 AS d4 ON f.d4 = d4.id
        JOIN starjoin_other_d5 AS d5 ON f.d5 = d5.id
        JOIN starjoin_other_d6 AS d6 ON f.d6 = d6.id) ON f.id = c1.id
$$ AS q_star_second \gset

SELECT starjoin_other_memory(:'q_star_second', true)
       < starjoin_other_memory(:'q_star_second', false) * 0.5 AS star_second_pruned;

--
-- Eager aggregation
--
-- With GROUP BY on a column of a dimension (h1), eager aggregation can place
-- a partial aggregate on the join of the fact and the other dimensions, and
-- then join h1.  With the canonical order that join rel is only built if h1
-- is the last dimension, so the plan depends on the order of the dimensions
-- in the FROM clause.
--
SELECT $$
SELECT h1.v, sum(hf.x) FROM starjoin_other_hf AS hf
  JOIN starjoin_other_h1 AS h1 ON hf.d1 = h1.id
  JOIN starjoin_other_h2 AS h2 ON hf.d2 = h2.id
  JOIN starjoin_other_h3 AS h3 ON hf.d3 = h3.id
 GROUP BY h1.v
$$ AS q_eager_first \gset

SELECT $$
SELECT h1.v, sum(hf.x) FROM starjoin_other_hf AS hf
  JOIN starjoin_other_h2 AS h2 ON hf.d2 = h2.id
  JOIN starjoin_other_h3 AS h3 ON hf.d3 = h3.id
  JOIN starjoin_other_h1 AS h1 ON hf.d1 = h1.id
 GROUP BY h1.v
$$ AS q_eager_last \gset

SELECT starjoin_other_plan(:'q_eager_first', false)
       LIKE '%"Partial Mode": "Partial"%' AS eager_first_partial_agg_off;
-- the canonical order joins h1 first, so there's no partial aggregate with
-- the GUC enabled
SELECT starjoin_other_plan(:'q_eager_first', true)
       LIKE '%"Partial Mode": "Partial"%' AS eager_first_partial_agg_on;
SELECT starjoin_other_plan(:'q_eager_last', true)
       LIKE '%"Partial Mode": "Partial"%' AS eager_last_partial_agg_on;

--
-- Rels without join clauses
--
-- A rel without join clauses can only be joined by a cartesian product.
-- Joining it to the fact multiplies the rows for all the following joins,
-- while joining it to a dimension (in the inner of the hash join of the
-- dimension to the fact) may be much cheaper.  So a dimension may be joined
-- to such rels before the fact.  Here two scans of a table with 2 rows (s1
-- and s2) have no join clauses, and the best plan joins their product to h1,
-- the last dimension in the canonical order.
--
SELECT $$
SELECT count(*) FROM starjoin_other_hf AS hf
  JOIN starjoin_other_h2 AS h2 ON hf.d2 = h2.id
  JOIN starjoin_other_h3 AS h3 ON hf.d3 = h3.id
  JOIN starjoin_other_h1 AS h1 ON hf.d1 = h1.id,
  starjoin_other_s AS s1, starjoin_other_s AS s2
$$ AS q_clauseless \gset

SELECT starjoin_other_cost(:'q_clauseless', true)
       <= starjoin_other_cost(:'q_clauseless', false) * 1.01 AS clauseless_cost_ok;

-- the same with a pseudoconstant qual, which is in the joininfo list of every
-- rel, but is not a join clause
SELECT starjoin_other_cost(:'q_clauseless' || $$WHERE now() > '2000-01-01'$$, true)
       <= starjoin_other_cost(:'q_clauseless' || $$WHERE now() > '2000-01-01'$$, false) * 1.01
       AS clauseless_where_now_cost_ok;

-- The dimension still gets joined to the fact in the canonical order.  With
-- h1 as the first dimension, the joins to h2 and h3 process the multiplied
-- rows, so the plan with the GUC enabled is more expensive.
SELECT $$
SELECT count(*) FROM starjoin_other_hf AS hf
  JOIN starjoin_other_h1 AS h1 ON hf.d1 = h1.id
  JOIN starjoin_other_h2 AS h2 ON hf.d2 = h2.id
  JOIN starjoin_other_h3 AS h3 ON hf.d3 = h3.id,
  starjoin_other_s AS s1, starjoin_other_s AS s2
$$ AS q_clauseless_first \gset

SELECT starjoin_other_cost(:'q_clauseless_first', true)
       > starjoin_other_cost(:'q_clauseless_first', false) * 1.01 AS clauseless_first_cost_higher;

-- But without the fact, a dimension must not be joined to rels with join
-- clauses, or the search might consider each dimension with all subsets of
-- c1-c5.  A pseudoconstant qual is in the joininfo list of every rel, but it
-- is not a join clause, so it must not make the rels joinable to each other.
-- The rels c1-c5 are joinable to each other even without the qual, through
-- an equivalence class (including f.id) or through a join clause referencing
-- all of them, so the qual should not add any joinrels.
SELECT $$
SELECT count(*) FROM starjoin_other_f AS f
  JOIN starjoin_other_d1 AS d1 ON f.d1 = d1.id
  JOIN starjoin_other_d2 AS d2 ON f.d2 = d2.id
  JOIN starjoin_other_d3 AS d3 ON f.d3 = d3.id
  JOIN starjoin_other_d4 AS d4 ON f.d4 = d4.id
  JOIN starjoin_other_c1 AS c1 ON f.id = c1.v
  JOIN starjoin_other_c2 AS c2 ON c1.v = c2.v
  JOIN starjoin_other_c3 AS c3 ON c2.v = c3.v
  JOIN starjoin_other_c4 AS c4 ON c3.v = c4.v
  JOIN starjoin_other_c5 AS c5 ON c4.v = c5.v
$$ AS q_ec_joins \gset

SELECT $$
SELECT count(*) FROM starjoin_other_f AS f
  JOIN starjoin_other_d1 AS d1 ON f.d1 = d1.id
  JOIN starjoin_other_d2 AS d2 ON f.d2 = d2.id
  JOIN starjoin_other_d3 AS d3 ON f.d3 = d3.id
  JOIN starjoin_other_d4 AS d4 ON f.d4 = d4.id
  CROSS JOIN starjoin_other_c1 AS c1
  CROSS JOIN starjoin_other_c2 AS c2
  CROSS JOIN starjoin_other_c3 AS c3
  CROSS JOIN starjoin_other_c4 AS c4
  JOIN starjoin_other_c5 AS c5 ON f.id < c1.v + c2.v + c3.v + c4.v + c5.v
$$ AS q_joininfo \gset

-- single subproblems with 10 rels (below geqo_threshold)
SET join_collapse_limit = 20;
SELECT starjoin_other_memory(:'q_ec_joins' || $$WHERE now() > '2000-01-01'$$, true)
       < starjoin_other_memory(:'q_ec_joins', true) * 1.5
       AS ec_joins_where_now_memory_ok;
SELECT starjoin_other_memory(:'q_joininfo' || $$WHERE now() > '2000-01-01'$$, true)
       < starjoin_other_memory(:'q_joininfo', true) * 1.5
       AS joininfo_where_now_memory_ok;
RESET join_collapse_limit;

RESET max_parallel_workers_per_gather;

DROP FUNCTION starjoin_other_cost(text, bool);
DROP FUNCTION starjoin_other_memory(text, bool);
DROP FUNCTION starjoin_other_plan(text, bool);

DROP TABLE starjoin_other_pf, starjoin_other_p1, starjoin_other_p2,
  starjoin_other_p3, starjoin_other_wf, starjoin_other_wa, starjoin_other_wb,
  starjoin_other_wc, starjoin_other_hf, starjoin_other_h1, starjoin_other_h2,
  starjoin_other_h3, starjoin_other_f, starjoin_other_a, starjoin_other_d1,
  starjoin_other_d2, starjoin_other_d3, starjoin_other_d4, starjoin_other_d5,
  starjoin_other_d6, starjoin_other_d7, starjoin_other_d8, starjoin_other_d9,
  starjoin_other_d10, starjoin_other_c1, starjoin_other_c2, starjoin_other_c3,
  starjoin_other_c4, starjoin_other_c5, starjoin_other_c6, starjoin_other_c7,
  starjoin_other_c8, starjoin_other_s;
