--
-- SELECT statements
--

CREATE EXTENSION pg_stat_statements;
SET pg_stat_statements.track_utility = FALSE;
SET pg_stat_statements.track_planning = TRUE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

--
-- simple and compound statements
--
SELECT 1 AS "int";

-- LIMIT and OFFSET patterns
-- Try some query permutations which once produced identical query IDs
SELECT 1 AS "int" LIMIT 1;
SELECT 1 AS "int" LIMIT 2;
SELECT 1 AS "int" OFFSET 1;
SELECT 1 AS "int" OFFSET 2;
SELECT 1 AS "int" OFFSET 1 LIMIT 1;
SELECT 1 AS "int" OFFSET 2 LIMIT 2;
SELECT 1 AS "int" LIMIT 1 OFFSET 1;
SELECT 1 AS "int" LIMIT 3 OFFSET 3;
SELECT 1 AS "int" OFFSET 1 FETCH FIRST 2 ROW ONLY;
SELECT 1 AS "int" OFFSET 2 FETCH FIRST 3 ROW ONLY;

-- DISTINCT and ORDER BY patterns
-- Try some query permutations which once produced identical query IDs
SELECT DISTINCT 1 AS "int";
SELECT DISTINCT 2 AS "int";
SELECT 1 AS "int" ORDER BY 1;
SELECT 2 AS "int" ORDER BY 1;

/* this comment should not appear in the output */
SELECT 'hello'
  -- but this one will appear
  AS "text";

SELECT 'world' AS "text";

-- transaction
BEGIN;
SELECT 1 AS "int";
SELECT 'hello' AS "text";
COMMIT;

-- compound transaction
BEGIN \;
SELECT 2.0 AS "float" \;
SELECT 'world' AS "text" \;
COMMIT;

-- compound with empty statements and spurious leading spacing
\;\;   SELECT 3 + 3 \;\;\;   SELECT ' ' || ' !' \;\;   SELECT 1 + 4 \;;

-- non ;-terminated statements
SELECT 1 + 1 + 1 AS "add" \gset
SELECT :add + 1 + 1 AS "add" \;
SELECT :add + 1 + 1 AS "add" \gset

-- set operator
SELECT 1 AS i UNION SELECT 2 ORDER BY i;

-- ? operator
select '{"a":1, "b":2}'::jsonb ? 'b';

-- cte
WITH t(f) AS (
  VALUES (1.0), (2.0)
)
  SELECT f FROM t ORDER BY f;

-- prepared statement with parameter
PREPARE pgss_test (int) AS SELECT $1, 'test' LIMIT 1;
EXECUTE pgss_test(1);
DEALLOCATE pgss_test;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- normalization of constants and parameters, with constant locations
-- recorded one or more times.
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT WHERE '1' IN ('1'::int, '3'::int::text);
SELECT WHERE (1, 2) IN ((1, 2), (2, 3));
SELECT WHERE (3, 4) IN ((5, 6), (8, 7));
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- with the last element being an explicit function call with an argument, ensure
-- the normalization of the squashing interval is correct.
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT WHERE 1 IN (1, int4(1), int4(2));
SELECT WHERE 1 = ANY (ARRAY[1, int4(1), int4(2)]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- queries with locking clauses
--
CREATE TABLE pgss_a (id integer PRIMARY KEY);
CREATE TABLE pgss_b (id integer PRIMARY KEY, a_id integer REFERENCES pgss_a);

SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- control query
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id;

-- test range tables
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR UPDATE;
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR UPDATE OF pgss_a;
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR UPDATE OF pgss_b;
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR UPDATE OF pgss_a, pgss_b; -- matches plain "FOR UPDATE"
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR UPDATE OF pgss_b, pgss_a;

-- test strengths
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR NO KEY UPDATE;
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR SHARE;
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR KEY SHARE;

-- test wait policies
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR UPDATE NOWAIT;
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR UPDATE SKIP LOCKED;

SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

DROP TABLE pgss_a, pgss_b CASCADE;

--
-- access to pg_stat_statements_info view
--
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT dealloc FROM pg_stat_statements_info;

-- FROM [ONLY]
CREATE TABLE tbl_inh(id integer);
CREATE TABLE tbl_inh_1() INHERITS (tbl_inh);
INSERT INTO tbl_inh_1 SELECT 1;

SELECT * FROM tbl_inh;
SELECT * FROM ONLY tbl_inh;

SELECT COUNT(*) FROM pg_stat_statements WHERE query LIKE '%FROM%tbl_inh%';

-- WITH TIES
CREATE TABLE limitoption AS SELECT 0 AS val FROM generate_series(1, 10);
SELECT *
FROM limitoption
WHERE val < 2
ORDER BY val
FETCH FIRST 2 ROWS WITH TIES;

SELECT *
FROM limitoption
WHERE val < 2
ORDER BY val
FETCH FIRST 2 ROW ONLY;

SELECT COUNT(*) FROM pg_stat_statements WHERE query LIKE '%FETCH FIRST%';

-- GROUP BY, HAVING, GROUPING
SELECT COUNT(*) FROM (VALUES (1::INT, 2::INT)) AS t(a, b) GROUP BY a;
SELECT COUNT(*) FROM (VALUES (1::INT, 2::INT)) AS t(a, b) GROUP BY b;
SELECT COUNT(*) FROM (VALUES (1::INT, 2::INT)) AS t(a, b) GROUP BY a, b;
SELECT COUNT(*) FROM (VALUES (1::INT, 2::INT)) AS t(a, b) GROUP BY b, a;
SELECT COUNT(*) FROM (VALUES (1::INT, 2::INT)) AS t(a, b) GROUP BY GROUPING SETS(a, ());
SELECT COUNT(*) FROM (VALUES (1::INT, 2::INT)) AS t(a, b) GROUP BY GROUPING SETS(b, ());
SELECT COUNT(*) FROM (VALUES (1::INT, 2::INT)) AS t(a, b) GROUP BY a HAVING a = 1;
SELECT COUNT(*) FROM (VALUES (1::INT, 2::INT)) AS t(a, b) GROUP BY a HAVING a = 2;
SELECT COUNT(*) FROM (VALUES (1::INT, 2::INT)) AS t(a, b) GROUP BY b HAVING b = 1;
SELECT GROUPING(a) FROM (VALUES (1::INT, 2::INT)) AS t(a, b) GROUP BY a;
SELECT GROUPING(b) FROM (VALUES (1::INT, 2::INT)) AS t(a, b) GROUP BY b;
SELECT GROUPING(b) FROM (VALUES (1::INT, 2::INT)) AS t(a, b) GROUP BY a, b;
SELECT GROUPING(b) FROM (VALUES (1::INT, 2::INT)) AS t(a, b) GROUP BY b, a;
SELECT calls, query FROM pg_stat_statements WHERE query LIKE '%GROUP BY%' ORDER BY query COLLATE "C";

-- GROUP BY [DISTINCT]
SELECT a, b, c
FROM (VALUES (1, 2, 3), (4, NULL, 6), (7, 8, 9)) AS t (a, b, c)
GROUP BY ROLLUP(a, b), rollup(a, c)
ORDER BY a, b, c;
SELECT a, b, c
FROM (VALUES (1, 2, 3), (4, NULL, 6), (7, 8, 9)) AS t (a, b, c)
GROUP BY DISTINCT ROLLUP(a, b), rollup(a, c)
ORDER BY a, b, c;

SELECT COUNT(*) FROM pg_stat_statements WHERE query LIKE '%GROUP BY%ROLLUP%';

-- GROUPING SET agglevelsup
SELECT (
  SELECT (
    SELECT GROUPING(a,b) FROM (VALUES (1)) v2(c)
  ) FROM (VALUES (1,2)) v1(a,b) GROUP BY (a,b)
) FROM (VALUES(6,7)) v3(e,f) GROUP BY ROLLUP(e,f);
SELECT (
  SELECT (
    SELECT GROUPING(e,f) FROM (VALUES (1)) v2(c)
  ) FROM (VALUES (1,2)) v1(a,b) GROUP BY (a,b)
) FROM (VALUES(6,7)) v3(e,f) GROUP BY ROLLUP(e,f);

SELECT COUNT(*) FROM pg_stat_statements WHERE query LIKE '%SELECT GROUPING%';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- Temporary table with same name, re-created.
BEGIN;
  CREATE TEMP TABLE temp_t (id int) ON COMMIT DROP;
  SELECT * FROM temp_t;
COMMIT;
BEGIN;
  CREATE TEMP TABLE temp_t (id int) ON COMMIT DROP;
  SELECT * FROM temp_t;
COMMIT;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- search_path with various schemas and temporary tables
CREATE SCHEMA pgss_schema_1;
CREATE SCHEMA pgss_schema_2;
-- Same attributes.
CREATE TABLE pgss_schema_1.tab_search_same (a int, b int);
CREATE TABLE pgss_schema_2.tab_search_same (a int, b int);
CREATE TEMP TABLE tab_search_same (a int, b int);
-- Different number of attributes, mapping types
CREATE TABLE pgss_schema_1.tab_search_diff_1 (a int);
CREATE TABLE pgss_schema_2.tab_search_diff_1 (a int, b int);
CREATE TEMP TABLE tab_search_diff_1 (a int, b int, c int);
-- Same number of attributes, different types
CREATE TABLE pgss_schema_1.tab_search_diff_2 (a int);
CREATE TABLE pgss_schema_2.tab_search_diff_2 (a text);
CREATE TEMP TABLE tab_search_diff_2 (a bigint);
-- First permanent schema
SET search_path = 'pgss_schema_1';
SELECT count(*) FROM tab_search_same;
SELECT a, b FROM tab_search_same;
SELECT count(*) FROM tab_search_diff_1;
SELECT count(*) FROM tab_search_diff_2;
SELECT a FROM tab_search_diff_2 AS t1;
SELECT a FROM tab_search_diff_2;
SELECT a AS a1 FROM tab_search_diff_2;
-- Second permanent schema
SET search_path = 'pgss_schema_2';
SELECT count(*) FROM tab_search_same;
SELECT a, b FROM tab_search_same;
SELECT count(*) FROM tab_search_diff_1;
SELECT count(*) FROM tab_search_diff_2;
SELECT a FROM tab_search_diff_2 AS t1;
SELECT a FROM tab_search_diff_2;
SELECT a AS a1 FROM tab_search_diff_2;
-- Temporary schema
SET search_path = 'pg_temp';
SELECT count(*) FROM tab_search_same;
SELECT a, b FROM tab_search_same;
SELECT count(*) FROM tab_search_diff_1;
SELECT count(*) FROM tab_search_diff_2;
SELECT a FROM tab_search_diff_2 AS t1;
SELECT a FROM tab_search_diff_2;
SELECT a AS a1 FROM tab_search_diff_2;
RESET search_path;
-- Schema qualifications
SELECT count(*) FROM pgss_schema_1.tab_search_same;
SELECT a, b FROM pgss_schema_1.tab_search_same;
SELECT count(*) FROM pgss_schema_2.tab_search_diff_1;
SELECT count(*) FROM pg_temp.tab_search_diff_2;
SELECT a FROM pgss_schema_2.tab_search_diff_2 AS t1;
SELECT a FROM pgss_schema_2.tab_search_diff_2;
SELECT a AS a1 FROM pgss_schema_2.tab_search_diff_2;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";
DROP SCHEMA pgss_schema_1 CASCADE;
DROP SCHEMA pgss_schema_2 CASCADE;
DROP TABLE tab_search_same, tab_search_diff_1, tab_search_diff_2;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- ROLLUP, CUBE and GROUPING SETS
CREATE TEMP TABLE pgss_empty_tab (a int, b int);
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT a, b, COUNT(*) FROM pgss_empty_tab GROUP BY ROLLUP (a, b);
SELECT a, b, COUNT(*) FROM pgss_empty_tab GROUP BY CUBE (a, b);
SELECT a, b, COUNT(*) FROM pgss_empty_tab GROUP BY GROUPING SETS (a, b);
SELECT a, b, COUNT(*) FROM pgss_empty_tab GROUP BY ROLLUP ((a, b));
SELECT a, b, COUNT(*) FROM pgss_empty_tab GROUP BY CUBE ((a, b));
SELECT a, b, COUNT(*) FROM pgss_empty_tab GROUP BY a, ROLLUP (b);
SELECT a, b, COUNT(*) FROM pgss_empty_tab GROUP BY a, CUBE (b);
SELECT a, b, COUNT(*) FROM pgss_empty_tab GROUP BY GROUPING SETS (ROLLUP (a), CUBE (b));
SELECT a, b, COUNT(*) FROM pgss_empty_tab GROUP BY GROUPING SETS (CUBE (a), ROLLUP (b));
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- WITH RECURSIVE ... SEARCH and CYCLE
WITH RECURSIVE t AS (SELECT a, b FROM pgss_empty_tab UNION ALL SELECT a + 1, b FROM t)
  SELECT * FROM t;
WITH RECURSIVE t AS (SELECT a, b FROM pgss_empty_tab UNION ALL SELECT a + 1, b FROM t)
  SEARCH DEPTH FIRST BY a SET seq SELECT * FROM t;
WITH RECURSIVE t AS (SELECT a, b FROM pgss_empty_tab UNION ALL SELECT a + 1, b FROM t)
  SEARCH BREADTH FIRST BY a SET seq SELECT * FROM t;
WITH RECURSIVE t AS (SELECT a, b FROM pgss_empty_tab UNION ALL SELECT a + 1, b FROM t)
  SEARCH DEPTH FIRST BY b SET seq SELECT * FROM t;
WITH RECURSIVE t AS (SELECT a, b FROM pgss_empty_tab UNION ALL SELECT a + 1, b FROM t)
  SEARCH DEPTH FIRST BY a, b SET seq SELECT * FROM t;
-- only the name of the sequence column differs
WITH RECURSIVE t AS (SELECT a, b FROM pgss_empty_tab UNION ALL SELECT a + 1, b FROM t)
  SEARCH DEPTH FIRST BY a SET ord SELECT * FROM t;
WITH RECURSIVE t AS (SELECT a, b FROM pgss_empty_tab UNION ALL SELECT a + 1, b FROM t)
  CYCLE a SET is_cycle USING path SELECT * FROM t;
WITH RECURSIVE t AS (SELECT a, b FROM pgss_empty_tab UNION ALL SELECT a + 1, b FROM t)
  CYCLE b SET is_cycle USING path SELECT * FROM t;
WITH RECURSIVE t AS (SELECT a, b FROM pgss_empty_tab UNION ALL SELECT a + 1, b FROM t)
  CYCLE a, b SET is_cycle USING path SELECT * FROM t;
-- TO true DEFAULT false is the default
WITH RECURSIVE t AS (SELECT a, b FROM pgss_empty_tab UNION ALL SELECT a + 1, b FROM t)
  CYCLE a SET is_cycle TO true DEFAULT false USING path SELECT * FROM t;
WITH RECURSIVE t AS (SELECT a, b FROM pgss_empty_tab UNION ALL SELECT a + 1, b FROM t)
  CYCLE a SET is_cycle TO 'Y' DEFAULT 'N' USING path SELECT * FROM t;
-- only the names of the mark and path columns differ
WITH RECURSIVE t AS (SELECT a, b FROM pgss_empty_tab UNION ALL SELECT a + 1, b FROM t)
  CYCLE a SET c USING p SELECT * FROM t;
WITH RECURSIVE t AS (SELECT a, b FROM pgss_empty_tab UNION ALL SELECT a + 1, b FROM t)
  SEARCH DEPTH FIRST BY a SET seq CYCLE a SET is_cycle USING path SELECT * FROM t;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- column definition lists
SELECT * FROM json_to_record('{"a": 1}') AS t(a int);
-- only the constant differs
SELECT * FROM json_to_record('{"a": 2}') AS t(a int);
SELECT * FROM json_to_record('{"a": 1}') AS t(a text);
SELECT * FROM json_to_record('{"a": 1}') AS t(a varchar(3));
SELECT * FROM json_to_record('{"a": 1}') AS t(a varchar(4));
SELECT * FROM json_to_record('{"a": 1}') AS t(a text COLLATE "C");
-- the column names are the keys to look up
SELECT * FROM json_to_record('{"a": 1}') AS t(b int);
SELECT * FROM json_to_record('{"a": 1}') AS t(a int, b int);
SELECT * FROM ROWS FROM (json_to_record('{"a": 1}') AS (a int),
  json_to_record('{"b": 1}') AS (b int)) AS t;
SELECT * FROM ROWS FROM (json_to_record('{"a": 1}') AS (a int),
  json_to_record('{"b": 1}') AS (b text)) AS t;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- VARIADIC
SELECT format('%s', VARIADIC ARRAY['a', 'b']);
SELECT format('%s', ARRAY['a', 'b']);
SELECT concat(VARIADIC ARRAY['a', 'b']);
SELECT concat(ARRAY['a', 'b']);
-- VARIADIC is the same as separate arguments, except for VARIADIC "any"
SELECT jsonb_extract_path('{}', a::text, b::text) FROM pgss_empty_tab;
SELECT jsonb_extract_path('{}', VARIADIC ARRAY[a::text, b::text])
  FROM pgss_empty_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- row constructors and row comparisons
CREATE TYPE pgss_pair1 AS (x int, y int);
CREATE TYPE pgss_pair2 AS (p int, q int);
SELECT (ROW(1, 2)::pgss_pair1).*;
SELECT (ROW(1, 2)::pgss_pair2).*;
SELECT ROW(1, 2);
SELECT ROW(1, 2)::pgss_pair1;
SELECT ROW(s, t) < ROW('b', 'b') FROM (VALUES ('a', 'a')) AS v(s, t);
SELECT ROW(s, t) ~<~ ROW('b', 'b') FROM (VALUES ('a', 'a')) AS v(s, t);
SELECT ROW(s, t) <= ROW('b', 'b') FROM (VALUES ('a', 'a')) AS v(s, t);
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";
DROP TYPE pgss_pair1, pgss_pair2;
DROP TABLE pgss_empty_tab;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
