--
-- Tests for OR-clause expansion into UNION ALL Append paths
--

CREATE TABLE orexp_a (id int, val int);
CREATE TABLE orexp_b (x int, y int, val int);

INSERT INTO orexp_a VALUES
  (1, 10),
  (2, 20),
  (3, 30),
  (4, NULL),
  (5, 50);

INSERT INTO orexp_b VALUES
  (1, 99, 100),
  (99, 2, 200),
  (3, 3, 300),
  (4, 99, 400),
  (99, 99, 500),
  (NULL, NULL, 600);

-- check GUC parameter
SHOW or_expansion_limit;

--
-- Disjunctive join conditions
--

-- disjunction of join keys
EXPLAIN (COSTS OFF)
SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

-- ensure results match when disabled
SET or_expansion_limit = 0;
EXPLAIN (COSTS OFF)
SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

RESET or_expansion_limit;

--
-- Cross-table filter predicates
--

-- cross-table disjunction with separate join condition
EXPLAIN (COSTS OFF)
SELECT a.id, a.val, b.val FROM orexp_a a JOIN orexp_b b ON a.id = b.x
WHERE a.val = 10 OR b.val = 300
ORDER BY a.id, a.val, b.val;

SELECT a.id, a.val, b.val FROM orexp_a a JOIN orexp_b b ON a.id = b.x
WHERE a.val = 10 OR b.val = 300
ORDER BY a.id, a.val, b.val;

--
-- Aggregation and grouping
--

-- GROUP BY over an expanded join
EXPLAIN (COSTS OFF)
SELECT a.id, count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
GROUP BY a.id
ORDER BY a.id;

SELECT a.id, count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
GROUP BY a.id
ORDER BY a.id;

-- zero-column target
EXPLAIN (COSTS OFF)
SELECT count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y;

SELECT count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y;

-- non-aggregate HAVING clause
EXPLAIN (COSTS OFF)
SELECT a.id, count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
GROUP BY a.id
HAVING a.id > 1
ORDER BY a.id;

-- aggregate HAVING clause
EXPLAIN (COSTS OFF)
SELECT a.id, count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
GROUP BY a.id
HAVING count(*) > 1
ORDER BY a.id;

-- grouping sets
EXPLAIN (COSTS OFF)
SELECT a.id, b.x, grouping (a.id), count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
GROUP BY GROUPING SETS ((a.id), (b.x), ())
ORDER BY 1, 2, 3, 4;

SELECT a.id, b.x, grouping (a.id), count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
GROUP BY GROUPING SETS ((a.id), (b.x), ())
ORDER BY 1, 2, 3, 4;

--
-- Subqueries, views, and derived tables
--

-- pulled-up simple subquery
EXPLAIN (COSTS OFF)
SELECT x.id, b.y FROM (SELECT id, val FROM orexp_a) x, orexp_b b
WHERE x.id = b.x OR x.val = b.y
ORDER BY 1, 2;

SELECT x.id, b.y FROM (SELECT id, val FROM orexp_a) x, orexp_b b
WHERE x.id = b.x OR x.val = b.y
ORDER BY 1, 2;

-- pulled-up subquery containing outer join
CREATE TABLE orexp_c (id int, val int);
CREATE TABLE orexp_d (id int, val int);
INSERT INTO orexp_c VALUES (1, 10), (2, 20), (3, NULL);
INSERT INTO orexp_d VALUES (1, 100), (3, 300), (5, 500);

EXPLAIN (COSTS OFF)
SELECT x.cid, b.y FROM (SELECT c.id AS cid, c.val AS cval, d.val + 1 AS dval
                        FROM orexp_c c LEFT JOIN orexp_d d ON c.id = d.id) x,
                      orexp_b b
WHERE x.dval = b.y OR x.cid = b.x
ORDER BY 1, 2;

SELECT x.cid, b.y FROM (SELECT c.id AS cid, c.val AS cval, d.val + 1 AS dval
                        FROM orexp_c c LEFT JOIN orexp_d d ON c.id = d.id) x,
                      orexp_b b
WHERE x.dval = b.y OR x.cid = b.x
ORDER BY 1, 2;

-- view expanded into pulled-up subquery
CREATE VIEW orexp_v AS
  SELECT a.id AS aid, a.val AS aval, b.x AS bx FROM orexp_a a, orexp_b b;

EXPLAIN (COSTS OFF)
SELECT v.aid, b.y FROM orexp_v v, orexp_b b
WHERE v.aval = b.y OR v.bx = b.y
ORDER BY 1, 2;

SELECT v.aid, b.y FROM orexp_v v, orexp_b b
WHERE v.aval = b.y OR v.bx = b.y
ORDER BY 1, 2;

-- non-table RTE_RESULT derived table
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b, (SELECT 1 AS one) x
WHERE a.id = b.x OR a.id = b.y
ORDER BY 1, 2;

--
-- Non-table range table entries
--

-- VALUES RTE with nulls
EXPLAIN (COSTS OFF)
SELECT v.a, b.y FROM (VALUES (1, 99), (2, NULL), (3, 3), (NULL, 4)) v (a, b), orexp_b b
WHERE v.a = b.x OR v.b = b.y
ORDER BY 1, 2;

SELECT v.a, b.y FROM (VALUES (1, 99), (2, NULL), (3, 3), (NULL, 4)) v (a, b), orexp_b b
WHERE v.a = b.x OR v.b = b.y
ORDER BY 1, 2;

-- set-returning function RTEs
EXPLAIN (COSTS OFF)
SELECT g, b.y FROM generate_series(1, 4) g, orexp_b b
WHERE g = b.x OR g = b.y
ORDER BY 1, 2;

SELECT g, b.y FROM generate_series(1, 4) g, orexp_b b
WHERE g = b.x OR g = b.y
ORDER BY 1, 2;

-- table functions
EXPLAIN (COSTS OFF)
SELECT jt.a, b.y FROM json_table('{"a": 1}'::jsonb, '$' COLUMNS (a int PATH '$.a')) jt, orexp_b b
WHERE jt.a = b.x OR jt.a = b.y;

SELECT jt.a, b.y FROM json_table('{"a": 1}'::jsonb, '$' COLUMNS (a int PATH '$.a')) jt, orexp_b b
WHERE jt.a = b.x OR jt.a = b.y;

--
-- Pseudoconstant predicates
--

-- arm with pseudoconstant qual
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR (b.y = 2 AND now() > '2000-01-01 00:00:00'::timestamp);

SELECT count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR (b.y = 2 AND now() > '2000-01-01 00:00:00'::timestamp);

-- pseudoconstant arm alongside subquery arm
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE (b.y = 2 AND now() > '2000-01-01 00:00:00'::timestamp)
   OR (a.id = b.x AND a.val > (SELECT count(*) FROM orexp_b));

SELECT count(*) FROM orexp_a a, orexp_b b
WHERE (b.y = 2 AND now() > '2000-01-01 00:00:00'::timestamp)
   OR (a.id = b.x AND a.val > (SELECT count(*) FROM orexp_b));

--
-- Outer joins
--

-- FULL JOIN with strict WHERE quals
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a FULL JOIN orexp_b b ON a.id = b.x
WHERE a.id = b.y OR b.x = a.id
ORDER BY 1, 2;

SELECT a.id, b.y FROM orexp_a a FULL JOIN orexp_b b ON a.id = b.x
WHERE a.id = b.y OR b.x = a.id
ORDER BY 1, 2;

-- LEFT JOIN with cross-relation disjunction in WHERE
EXPLAIN (COSTS OFF)
SELECT a.id, b.x, b.y FROM orexp_a a LEFT JOIN orexp_b b ON (a.id = b.x OR a.val = b.y)
WHERE b.val = 100 OR a.id = 1
ORDER BY 1, 2, 3;

SELECT a.id, b.x, b.y FROM orexp_a a LEFT JOIN orexp_b b ON (a.id = b.x OR a.val = b.y)
WHERE b.val = 100 OR a.id = 1
ORDER BY 1, 2, 3;

--
-- Queries ineligible for OR expansion
--

-- volatile set-returning function
EXPLAIN (COSTS OFF)
SELECT g, b.y FROM generate_series(1, (random() * 3)::int) g, orexp_b b
WHERE g = b.x OR g = b.y;

-- volatile VALUES list
EXPLAIN (COSTS OFF)
SELECT v.a, b.y FROM (VALUES ((random() * 3)::int)) v (a), orexp_b b
WHERE v.a = b.x OR v.a = b.y;

-- volatile JSON_TABLE expressions
EXPLAIN (COSTS OFF)
SELECT jt.a, b.y FROM json_table(('{"a":' || (random() * 2)::int || '}')::jsonb,
                                 '$' COLUMNS (a int PATH '$.a')) jt, orexp_b b
WHERE jt.a = b.x OR jt.a = b.y;

-- volatile function in WHERE
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR (a.id = b.y AND random() < 0.5);

-- correlated SubPlan in WHERE
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE (a.id = b.x OR a.id = b.y)
  AND a.val > (SELECT max(b2.val) FROM orexp_b b2 WHERE b2.x = a.id);

-- TABLESAMPLE
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b TABLESAMPLE SYSTEM (50)
WHERE a.id = b.x OR a.id = b.y;

-- unreduced FULL JOIN
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a FULL JOIN orexp_b b ON a.id = b.x
WHERE a.val IS NULL OR b.val IS NULL;

-- lateral reference
EXPLAIN (COSTS OFF)
SELECT a.id, l.ly FROM orexp_a a,
       LATERAL (SELECT b.y AS ly FROM orexp_b b WHERE b.x = a.id OR b.y = a.val) l
WHERE a.id = l.ly OR a.val = 1;

-- unflattened subquery with grouping
EXPLAIN (COSTS OFF)
SELECT a.id, s.n FROM orexp_a a,
       (SELECT x AS sx, count(*) AS n FROM orexp_b GROUP BY x) s
WHERE a.id = s.sx OR a.val = s.n;

-- append relation from UNION ALL
EXPLAIN (COSTS OFF)
SELECT u.id, a.val FROM (SELECT x AS id, val FROM orexp_b
                         UNION ALL
                         SELECT id, val FROM orexp_a) u, orexp_a a
WHERE u.val = a.val OR u.id = a.id;

-- partitioned table
CREATE TABLE orexp_part (id int, d int) PARTITION BY RANGE (d);
CREATE TABLE orexp_part1 PARTITION OF orexp_part FOR VALUES FROM (0) TO (10);
CREATE TABLE orexp_part2 PARTITION OF orexp_part FOR VALUES FROM (10) TO (20);
INSERT INTO orexp_part VALUES (1, 5), (2, 15);

EXPLAIN (COSTS OFF)
SELECT a.id, p.d FROM orexp_a a, orexp_part p
WHERE a.id = p.id OR a.val = p.d;

-- table inheritance
CREATE TABLE orexp_inh_parent (id int, val int);
CREATE TABLE orexp_inh_child () INHERITS (orexp_inh_parent);
INSERT INTO orexp_inh_parent VALUES (1, 10);
INSERT INTO orexp_inh_child VALUES (2, 20);

EXPLAIN (COSTS OFF)
SELECT a.id, p.val FROM orexp_a a, orexp_inh_parent p
WHERE a.id = p.id OR a.val = p.val;

--
-- Permitted sublinks
--

-- uncorrelated scalar sublink
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE (a.id = b.x OR a.id = b.y)
  AND a.val > (SELECT min(val) FROM orexp_b WHERE val > 150)
ORDER BY 1, 2;

SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE (a.id = b.x OR a.id = b.y)
  AND a.val > (SELECT min(val) FROM orexp_b WHERE val > 150)
ORDER BY 1, 2;

--
-- Multiple OR clauses in WHERE
--

EXPLAIN (COSTS OFF)
SELECT a.id, b.val FROM orexp_a a, orexp_b b
WHERE (a.id = b.x OR a.id = b.y) AND (a.val = b.val OR a.val = b.x)
ORDER BY 1, 2;

SELECT a.id, b.val FROM orexp_a a, orexp_b b
WHERE (a.id = b.x OR a.id = b.y) AND (a.val = b.val OR a.val = b.x)
ORDER BY 1, 2;

--
-- Control via or_expansion_limit
--

-- arm count exceeds limit
SET or_expansion_limit = 2;
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y OR a.val = b.val
ORDER BY 1, 2;

-- arm count within limit
SET or_expansion_limit = 3;
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y OR a.val = b.val
ORDER BY 1, 2;

SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y OR a.val = b.val
ORDER BY 1, 2;

--
-- Clean up
--
RESET or_expansion_limit;
DROP VIEW orexp_v;
DROP TABLE orexp_c, orexp_d;
DROP TABLE orexp_part;
DROP TABLE orexp_inh_parent CASCADE;
DROP TABLE orexp_a, orexp_b;
