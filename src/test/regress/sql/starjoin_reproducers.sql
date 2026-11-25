--
-- Planning queries with enable_starjoin_ordering, where the join search
-- can't simply follow the canonical order
--
-- In queries A-I, the clusters (determined for the whole query) and the join
-- search subproblems (sub-joinlists, determined by
-- join_collapse_limit/from_collapse_limit, explicit joins, views and
-- subqueries) do not align, so the canonical order can't be followed in some
-- subproblem.  The canonical-order check must not reject all joins the
-- subproblem can build, otherwise the planning fails with "ERROR:  failed to
-- build any N-way joins".
--
-- In query J, the last-ditch pass would join dimensions to rels without the
-- fact (and two such joinrels would then get joined).  Those rels have join
-- clauses, so such joinrels are not allowed, and the join search must not
-- fail without them.
--
-- For each query, show the plan with the GUC enabled, and then check that
-- the query returns the same result with the GUC enabled and disabled.
--

CREATE TABLE starjoin_reproducers_d0 (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off);
CREATE TABLE starjoin_reproducers_d1 (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off);
CREATE TABLE starjoin_reproducers_d2 (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off);
CREATE TABLE starjoin_reproducers_d3 (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off);
CREATE TABLE starjoin_reproducers_d4 (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off);
CREATE TABLE starjoin_reproducers_d5 (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off);
CREATE TABLE starjoin_reproducers_d6 (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off);
CREATE TABLE starjoin_reproducers_d7 (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off);

CREATE TABLE starjoin_reproducers_f (id int PRIMARY KEY,
  d0 int NOT NULL REFERENCES starjoin_reproducers_d0,
  d1 int NOT NULL REFERENCES starjoin_reproducers_d1,
  d2 int NOT NULL REFERENCES starjoin_reproducers_d2,
  d3 int NOT NULL REFERENCES starjoin_reproducers_d3,
  d4 int NOT NULL REFERENCES starjoin_reproducers_d4,
  d5 int NOT NULL REFERENCES starjoin_reproducers_d5,
  d6 int NOT NULL REFERENCES starjoin_reproducers_d6,
  d7 int NOT NULL REFERENCES starjoin_reproducers_d7,
  x int) WITH (autovacuum_enabled = off);

INSERT INTO starjoin_reproducers_d0 SELECT i, i FROM generate_series(1, 10) i;
INSERT INTO starjoin_reproducers_d1 SELECT i, 10 * i FROM generate_series(1, 20) i;
INSERT INTO starjoin_reproducers_d2 SELECT i, 100 * i FROM generate_series(1, 30) i;
INSERT INTO starjoin_reproducers_d3 SELECT i, 1000 * i FROM generate_series(1, 40) i;
INSERT INTO starjoin_reproducers_d4 SELECT i, i FROM generate_series(1, 50) i;
INSERT INTO starjoin_reproducers_d5 SELECT i, 10 * i FROM generate_series(1, 60) i;
INSERT INTO starjoin_reproducers_d6 SELECT i, 100 * i FROM generate_series(1, 70) i;
INSERT INTO starjoin_reproducers_d7 SELECT i, 1000 * i FROM generate_series(1, 80) i;

INSERT INTO starjoin_reproducers_f
SELECT i, 1 + i % 10, 1 + (i * 3) % 20, 1 + (i * 7) % 30, 1 + (i * 11) % 40,
       1 + (i * 13) % 50, 1 + (i * 17) % 60, 1 + (i * 19) % 70, 1 + (i * 23) % 80, i
  FROM generate_series(1, 500) i;

-- small table for the outer joins (used by J)
CREATE TABLE starjoin_reproducers_t (a int, b int) WITH (autovacuum_enabled = off);
INSERT INTO starjoin_reproducers_t SELECT i % 2, i % 3 FROM generate_series(1, 4) i;

VACUUM ANALYZE starjoin_reproducers_d0, starjoin_reproducers_d1,
  starjoin_reproducers_d2, starjoin_reproducers_d3, starjoin_reproducers_d4,
  starjoin_reproducers_d5, starjoin_reproducers_d6, starjoin_reproducers_d7,
  starjoin_reproducers_f, starjoin_reproducers_t;

-- star over the fact and dimensions d1-d7 (used by D)
CREATE VIEW starjoin_reproducers_star AS
SELECT f.*, d1.v AS v1, d2.v AS v2, d3.v AS v3, d4.v AS v4, d5.v AS v5,
       d6.v AS v6, d7.v AS v7
  FROM starjoin_reproducers_f AS f
  JOIN starjoin_reproducers_d1 AS d1 ON f.d1 = d1.id
  JOIN starjoin_reproducers_d2 AS d2 ON f.d2 = d2.id
  JOIN starjoin_reproducers_d3 AS d3 ON f.d3 = d3.id
  JOIN starjoin_reproducers_d4 AS d4 ON f.d4 = d4.id
  JOIN starjoin_reproducers_d5 AS d5 ON f.d5 = d5.id
  JOIN starjoin_reproducers_d6 AS d6 ON f.d6 = d6.id
  JOIN starjoin_reproducers_d7 AS d7 ON f.d7 = d7.id;

-- fact joined to a single dimension d7 (used by H)
CREATE VIEW starjoin_reproducers_fv AS
SELECT f.*, d7.v AS v7
  FROM starjoin_reproducers_f AS f
  JOIN starjoin_reproducers_d7 AS d7 ON f.d7 = d7.id;

-- run the query with the GUC enabled and disabled, return both results
CREATE FUNCTION starjoin_reproducers_check(query text,
                                           OUT starjoin_on text,
                                           OUT starjoin_off text)
LANGUAGE plpgsql AS $$
BEGIN
  PERFORM set_config('enable_starjoin_ordering', 'on', true);
  EXECUTE format('SELECT row(q.*)::text FROM (%s) q', query) INTO starjoin_on;
  PERFORM set_config('enable_starjoin_ordering', 'off', true);
  EXECUTE format('SELECT row(q.*)::text FROM (%s) q', query) INTO starjoin_off;
END;
$$;

SET enable_starjoin_ordering = on;

-- A: explicit join order, the subproblem has the fact and dimensions that are
-- not a prefix of the canonical order (d1 is joined in the upper subproblem)
SET join_collapse_limit = 1;
EXPLAIN (COSTS OFF)
SELECT count(*), sum(d1.v + d2.v + d3.v)
  FROM starjoin_reproducers_d1 AS d1
  JOIN (starjoin_reproducers_f AS f
        JOIN starjoin_reproducers_d2 AS d2 ON f.d2 = d2.id
        JOIN starjoin_reproducers_d3 AS d3 ON f.d3 = d3.id) ON f.d1 = d1.id;
SELECT * FROM starjoin_reproducers_check($$
SELECT count(*), sum(d1.v + d2.v + d3.v)
  FROM starjoin_reproducers_d1 AS d1
  JOIN (starjoin_reproducers_f AS f
        JOIN starjoin_reproducers_d2 AS d2 ON f.d2 = d2.id
        JOIN starjoin_reproducers_d3 AS d3 ON f.d3 = d3.id) ON f.d1 = d1.id
$$);

-- B: explicit join order, the subproblem has dimensions but not the fact
EXPLAIN (COSTS OFF)
SELECT count(*), sum(d1.v + d2.v)
  FROM starjoin_reproducers_f AS f
  JOIN (starjoin_reproducers_d1 AS d1 CROSS JOIN starjoin_reproducers_d2 AS d2)
    ON f.d1 = d1.id AND f.d2 = d2.id;
SELECT * FROM starjoin_reproducers_check($$
SELECT count(*), sum(d1.v + d2.v)
  FROM starjoin_reproducers_f AS f
  JOIN (starjoin_reproducers_d1 AS d1 CROSS JOIN starjoin_reproducers_d2 AS d2)
    ON f.d1 = d1.id AND f.d2 = d2.id
$$);
RESET join_collapse_limit;

-- C: default settings, 9 rels, join_collapse_limit splits the problem, and
-- the first dimension (d0) is joined in the upper subproblem
EXPLAIN (COSTS OFF)
SELECT count(*), sum(d0.v + d7.v)
  FROM starjoin_reproducers_d0 AS d0
  JOIN (starjoin_reproducers_f AS f
        JOIN starjoin_reproducers_d1 AS d1 ON f.d1 = d1.id
        JOIN starjoin_reproducers_d2 AS d2 ON f.d2 = d2.id
        JOIN starjoin_reproducers_d3 AS d3 ON f.d3 = d3.id
        JOIN starjoin_reproducers_d4 AS d4 ON f.d4 = d4.id
        JOIN starjoin_reproducers_d5 AS d5 ON f.d5 = d5.id
        JOIN starjoin_reproducers_d6 AS d6 ON f.d6 = d6.id
        JOIN starjoin_reproducers_d7 AS d7 ON f.d7 = d7.id) ON f.d0 = d0.id;
SELECT * FROM starjoin_reproducers_check($$
SELECT count(*), sum(d0.v + d7.v)
  FROM starjoin_reproducers_d0 AS d0
  JOIN (starjoin_reproducers_f AS f
        JOIN starjoin_reproducers_d1 AS d1 ON f.d1 = d1.id
        JOIN starjoin_reproducers_d2 AS d2 ON f.d2 = d2.id
        JOIN starjoin_reproducers_d3 AS d3 ON f.d3 = d3.id
        JOIN starjoin_reproducers_d4 AS d4 ON f.d4 = d4.id
        JOIN starjoin_reproducers_d5 AS d5 ON f.d5 = d5.id
        JOIN starjoin_reproducers_d6 AS d6 ON f.d6 = d6.id
        JOIN starjoin_reproducers_d7 AS d7 ON f.d7 = d7.id) ON f.d0 = d0.id
$$);

-- D: default settings, view over the star, joined to another dimension
EXPLAIN (COSTS OFF)
SELECT count(*), sum(d0.v + s.v1 + s.v7)
  FROM starjoin_reproducers_d0 AS d0
  JOIN starjoin_reproducers_star AS s ON s.d0 = d0.id;
SELECT * FROM starjoin_reproducers_check($$
SELECT count(*), sum(d0.v + s.v1 + s.v7)
  FROM starjoin_reproducers_d0 AS d0
  JOIN starjoin_reproducers_star AS s ON s.d0 = d0.id
$$);

-- E: default settings, FROM list with a pulled-up subquery, split by
-- from_collapse_limit
EXPLAIN (COSTS OFF)
SELECT count(*), sum(d0.v + s.v6 + s.v7)
  FROM starjoin_reproducers_f AS f,
       starjoin_reproducers_d0 AS d0, starjoin_reproducers_d1 AS d1,
       starjoin_reproducers_d2 AS d2, starjoin_reproducers_d3 AS d3,
       starjoin_reproducers_d4 AS d4, starjoin_reproducers_d5 AS d5,
       (SELECT d6.id AS i6, d6.v AS v6, d7.id AS i7, d7.v AS v7
          FROM starjoin_reproducers_d6 AS d6,
               starjoin_reproducers_d7 AS d7) AS s
 WHERE f.d0 = d0.id AND f.d1 = d1.id AND f.d2 = d2.id AND f.d3 = d3.id
   AND f.d4 = d4.id AND f.d5 = d5.id AND f.d6 = s.i6 AND f.d7 = s.i7;
SELECT * FROM starjoin_reproducers_check($$
SELECT count(*), sum(d0.v + s.v6 + s.v7)
  FROM starjoin_reproducers_f AS f,
       starjoin_reproducers_d0 AS d0, starjoin_reproducers_d1 AS d1,
       starjoin_reproducers_d2 AS d2, starjoin_reproducers_d3 AS d3,
       starjoin_reproducers_d4 AS d4, starjoin_reproducers_d5 AS d5,
       (SELECT d6.id AS i6, d6.v AS v6, d7.id AS i7, d7.v AS v7
          FROM starjoin_reproducers_d6 AS d6,
               starjoin_reproducers_d7 AS d7) AS s
 WHERE f.d0 = d0.id AND f.d1 = d1.id AND f.d2 = d2.id AND f.d3 = d3.id
   AND f.d4 = d4.id AND f.d5 = d5.id AND f.d6 = s.i6 AND f.d7 = s.i7
$$);

-- F: from_collapse_limit = 3, FROM list with an explicit join of the fact
-- and a dimension with higher relid than the dimensions in the FROM list
SET from_collapse_limit = 3;
EXPLAIN (COSTS OFF)
SELECT count(*), sum(d1.v + d2.v + d3.v)
  FROM starjoin_reproducers_d1 AS d1, starjoin_reproducers_d2 AS d2,
       starjoin_reproducers_f AS f
       JOIN starjoin_reproducers_d3 AS d3 ON f.d3 = d3.id
 WHERE f.d1 = d1.id AND f.d2 = d2.id;
SELECT * FROM starjoin_reproducers_check($$
SELECT count(*), sum(d1.v + d2.v + d3.v)
  FROM starjoin_reproducers_d1 AS d1, starjoin_reproducers_d2 AS d2,
       starjoin_reproducers_f AS f
       JOIN starjoin_reproducers_d3 AS d3 ON f.d3 = d3.id
 WHERE f.d1 = d1.id AND f.d2 = d2.id
$$);
RESET from_collapse_limit;

-- G: default settings, 7 dimensions in the FROM list, and an explicit join
-- of the fact and the last dimension (9 rels > from_collapse_limit)
EXPLAIN (COSTS OFF)
SELECT count(*), sum(d0.v + d6.v + d7.v)
  FROM starjoin_reproducers_d0 AS d0, starjoin_reproducers_d1 AS d1,
       starjoin_reproducers_d2 AS d2, starjoin_reproducers_d3 AS d3,
       starjoin_reproducers_d4 AS d4, starjoin_reproducers_d5 AS d5,
       starjoin_reproducers_d6 AS d6,
       starjoin_reproducers_f AS f
       JOIN starjoin_reproducers_d7 AS d7 ON f.d7 = d7.id
 WHERE f.d0 = d0.id AND f.d1 = d1.id AND f.d2 = d2.id AND f.d3 = d3.id
   AND f.d4 = d4.id AND f.d5 = d5.id AND f.d6 = d6.id;
SELECT * FROM starjoin_reproducers_check($$
SELECT count(*), sum(d0.v + d6.v + d7.v)
  FROM starjoin_reproducers_d0 AS d0, starjoin_reproducers_d1 AS d1,
       starjoin_reproducers_d2 AS d2, starjoin_reproducers_d3 AS d3,
       starjoin_reproducers_d4 AS d4, starjoin_reproducers_d5 AS d5,
       starjoin_reproducers_d6 AS d6,
       starjoin_reproducers_f AS f
       JOIN starjoin_reproducers_d7 AS d7 ON f.d7 = d7.id
 WHERE f.d0 = d0.id AND f.d1 = d1.id AND f.d2 = d2.id AND f.d3 = d3.id
   AND f.d4 = d4.id AND f.d5 = d5.id AND f.d6 = d6.id
$$);

-- H: default settings, view over the fact and the last dimension, joined to
-- the 7 other dimensions
EXPLAIN (COSTS OFF)
SELECT count(*), sum(d0.v + d6.v + fv.v7)
  FROM starjoin_reproducers_d0 AS d0, starjoin_reproducers_d1 AS d1,
       starjoin_reproducers_d2 AS d2, starjoin_reproducers_d3 AS d3,
       starjoin_reproducers_d4 AS d4, starjoin_reproducers_d5 AS d5,
       starjoin_reproducers_d6 AS d6, starjoin_reproducers_fv AS fv
 WHERE fv.d0 = d0.id AND fv.d1 = d1.id AND fv.d2 = d2.id AND fv.d3 = d3.id
   AND fv.d4 = d4.id AND fv.d5 = d5.id AND fv.d6 = d6.id;
SELECT * FROM starjoin_reproducers_check($$
SELECT count(*), sum(d0.v + d6.v + fv.v7)
  FROM starjoin_reproducers_d0 AS d0, starjoin_reproducers_d1 AS d1,
       starjoin_reproducers_d2 AS d2, starjoin_reproducers_d3 AS d3,
       starjoin_reproducers_d4 AS d4, starjoin_reproducers_d5 AS d5,
       starjoin_reproducers_d6 AS d6, starjoin_reproducers_fv AS fv
 WHERE fv.d0 = d0.id AND fv.d1 = d1.id AND fv.d2 = d2.id AND fv.d3 = d3.id
   AND fv.d4 = d4.id AND fv.d5 = d5.id AND fv.d6 = d6.id
$$);

-- I: join_collapse_limit = 3, right-nested explicit joins
SET join_collapse_limit = 3;
EXPLAIN (COSTS OFF)
SELECT count(*), sum(d1.v + d2.v + d3.v + d4.v)
  FROM starjoin_reproducers_d1 AS d1
  JOIN (starjoin_reproducers_d2 AS d2
        JOIN (starjoin_reproducers_f AS f
              JOIN starjoin_reproducers_d3 AS d3 ON f.d3 = d3.id
              JOIN starjoin_reproducers_d4 AS d4 ON f.d4 = d4.id)
          ON f.d2 = d2.id)
    ON f.d1 = d1.id;
SELECT * FROM starjoin_reproducers_check($$
SELECT count(*), sum(d1.v + d2.v + d3.v + d4.v)
  FROM starjoin_reproducers_d1 AS d1
  JOIN (starjoin_reproducers_d2 AS d2
        JOIN (starjoin_reproducers_f AS f
              JOIN starjoin_reproducers_d3 AS d3 ON f.d3 = d3.id
              JOIN starjoin_reproducers_d4 AS d4 ON f.d4 = d4.id)
          ON f.d2 = d2.id)
    ON f.d1 = d1.id
$$);
RESET join_collapse_limit;

-- J: from_collapse_limit = 9, the subquery is a separate subproblem with the
-- star (the fact has a join clause only to w, outside the subquery) and two
-- outer joins of three rels each, linked by a non-strict clause between their
-- nullable sides.  Nothing can be joined at level 4, so the last-ditch pass
-- joins the outer joins to other rels, but not to the dimensions (without the
-- fact, a dimension may be joined only to rels without join clauses).
SET from_collapse_limit = 9;
EXPLAIN (COSTS OFF)
SELECT count(*)
  FROM starjoin_reproducers_t AS w,
       (SELECT f.x
          FROM starjoin_reproducers_f AS f
          JOIN starjoin_reproducers_d1 AS d1 ON f.d1 = d1.id
          JOIN starjoin_reproducers_d2 AS d2 ON f.d2 = d2.id,
               starjoin_reproducers_t AS p
          LEFT JOIN (starjoin_reproducers_t AS q
                     JOIN starjoin_reproducers_t AS q2 ON q.a = q2.a)
            ON p.a = q.a,
               starjoin_reproducers_t AS s
          LEFT JOIN (starjoin_reproducers_t AS t
                     JOIN starjoin_reproducers_t AS t2 ON t.a = t2.a)
            ON s.a = t.a
         WHERE f.id <= 10 AND coalesce(q.b, 0) = coalesce(t.b, 0)) AS sub
 WHERE sub.x = w.a;
SELECT * FROM starjoin_reproducers_check($$
SELECT count(*)
  FROM starjoin_reproducers_t AS w,
       (SELECT f.x
          FROM starjoin_reproducers_f AS f
          JOIN starjoin_reproducers_d1 AS d1 ON f.d1 = d1.id
          JOIN starjoin_reproducers_d2 AS d2 ON f.d2 = d2.id,
               starjoin_reproducers_t AS p
          LEFT JOIN (starjoin_reproducers_t AS q
                     JOIN starjoin_reproducers_t AS q2 ON q.a = q2.a)
            ON p.a = q.a,
               starjoin_reproducers_t AS s
          LEFT JOIN (starjoin_reproducers_t AS t
                     JOIN starjoin_reproducers_t AS t2 ON t.a = t2.a)
            ON s.a = t.a
         WHERE f.id <= 10 AND coalesce(q.b, 0) = coalesce(t.b, 0)) AS sub
 WHERE sub.x = w.a
$$);
RESET from_collapse_limit;

RESET enable_starjoin_ordering;

DROP FUNCTION starjoin_reproducers_check(text);
DROP VIEW starjoin_reproducers_star, starjoin_reproducers_fv;
DROP TABLE starjoin_reproducers_f, starjoin_reproducers_d0,
  starjoin_reproducers_d1, starjoin_reproducers_d2, starjoin_reproducers_d3,
  starjoin_reproducers_d4, starjoin_reproducers_d5, starjoin_reproducers_d6,
  starjoin_reproducers_d7, starjoin_reproducers_t;
