--
-- SQL/JSON
--

SET pg_stat_statements.track_utility = FALSE;

-- The table is kept empty, as only the query IDs and texts matter here.
CREATE TABLE pgss_sqljson_tab (js jsonb, t text, b bytea);

--
-- SQL/JSON query functions
--

-- JSON_VALUE ON EMPTY and ON ERROR behaviors
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT JSON_VALUE(js, '$.a') FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' NULL ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' ERROR ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' DEFAULT 1 ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.b' DEFAULT 2 ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' ERROR ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' DEFAULT 1 ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' NULL ON EMPTY ERROR ON ERROR) FROM pgss_sqljson_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- JSON_VALUE RETURNING and PASSING
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT JSON_VALUE(js, '$.a' RETURNING int) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' RETURNING text) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' RETURNING varchar(3)) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a ? (@ > $x)' PASSING 1 AS x) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a ? (@ > $x)' PASSING 2 AS x) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a ? (@ > $y)' PASSING 1 AS y) FROM pgss_sqljson_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- JSON_QUERY ON EMPTY and ON ERROR behaviors
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT JSON_QUERY(js, '$.a' EMPTY ARRAY ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' EMPTY OBJECT ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' EMPTY ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' NULL ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' ERROR ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' DEFAULT '[]' ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' EMPTY ARRAY ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' EMPTY OBJECT ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' ERROR ON ERROR) FROM pgss_sqljson_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- JSON_QUERY WRAPPER, QUOTES, RETURNING and FORMAT JSON
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT JSON_QUERY(js, '$.a' WITHOUT WRAPPER) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' WITH WRAPPER) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' WITH UNCONDITIONAL WRAPPER) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' WITH CONDITIONAL WRAPPER) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' KEEP QUOTES) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' OMIT QUOTES) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' RETURNING text) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' RETURNING json) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' RETURNING text FORMAT JSON) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(t, '$.a') FROM pgss_sqljson_tab;
SELECT JSON_QUERY(t FORMAT JSON, '$.a') FROM pgss_sqljson_tab;
SELECT JSON_QUERY(b FORMAT JSON, '$.a') FROM pgss_sqljson_tab;
SELECT JSON_QUERY(b FORMAT JSON ENCODING UTF8, '$.a') FROM pgss_sqljson_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- JSON_EXISTS ON ERROR behaviors and PASSING
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT JSON_EXISTS(js, 'strict $.a' TRUE ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_EXISTS(js, 'strict $.a' FALSE ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_EXISTS(js, 'strict $.a' UNKNOWN ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_EXISTS(js, 'strict $.a' ERROR ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_EXISTS(js, '$.a ? (@ > $x)' PASSING 1 AS x) FROM pgss_sqljson_tab;
SELECT JSON_EXISTS(js, '$.a ? (@ > $x)' PASSING 2 AS x) FROM pgss_sqljson_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- JSON_TABLE
--

-- column types, ON EMPTY and ON ERROR behaviors
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a text PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a varchar(3) PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a' ERROR ON EMPTY)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a' DEFAULT 1 ON EMPTY)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.b' DEFAULT 2 ON EMPTY)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a' ERROR ON ERROR)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a' DEFAULT 1 ON ERROR)) jt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- EXISTS, FORMAT JSON and FOR ORDINALITY columns
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int EXISTS PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int EXISTS PATH '$.a' TRUE ON ERROR)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int EXISTS PATH '$.a' FALSE ON ERROR)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a jsonb PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a jsonb FORMAT JSON PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a jsonb FORMAT JSON PATH '$.a' WITH WRAPPER)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a text FORMAT JSON PATH '$.a' OMIT QUOTES)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a jsonb FORMAT JSON PATH '$.a' EMPTY ARRAY ON EMPTY)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a jsonb FORMAT JSON PATH '$.a' EMPTY OBJECT ON EMPTY)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a FOR ORDINALITY)) jt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- column names and paths
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a')) jt;
-- only the column name and the path differ
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (b int PATH '$.b')) jt;
-- without PATH, the path is derived from the column name
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (b int)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a', b int PATH '$.b')) jt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- NESTED PATH
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a', b int PATH '$.b')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' COLUMNS (b int PATH '$'))) jt;
-- only the paths and the path name differ
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a',
    NESTED PATH '$.c[*]' AS p1 COLUMNS (b int PATH '$'))) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' COLUMNS (b int PATH '$'),
    NESTED PATH '$.c[*]' COLUMNS (c int PATH '$'))) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' COLUMNS (b int PATH '$',
      NESTED PATH '$.c[*]' COLUMNS (c int PATH '$')))) jt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- PLAN
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a') PLAN (p0)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'))) jt;
-- PLAN (p0 OUTER p1) is the default plan
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'))
    PLAN (p0 OUTER p1)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'))
    PLAN (p0 INNER p1)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'),
    NESTED PATH '$.c[*]' AS p2 COLUMNS (c int PATH '$'))
    PLAN (p0 OUTER (p1 UNION p2))) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'),
    NESTED PATH '$.c[*]' AS p2 COLUMNS (c int PATH '$'))
    PLAN (p0 OUTER (p1 CROSS p2))) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'),
    NESTED PATH '$.c[*]' AS p2 COLUMNS (c int PATH '$'))
    PLAN DEFAULT (INNER, CROSS)) jt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- table-level ON ERROR and PASSING
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a') EMPTY ON ERROR) jt;
-- EMPTY [ARRAY] ON ERROR is the default
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a') EMPTY ARRAY ON ERROR) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a') ERROR ON ERROR) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*] ? (@.a > $x)' PASSING 1 AS x
    COLUMNS (a int PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*] ? (@.a > $x)' PASSING 2 AS x
    COLUMNS (a int PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*] ? (@.a > $y)' PASSING 1 AS y
    COLUMNS (a int PATH '$.a')) jt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- SQL/JSON constructors and predicates
--

-- JSON_OBJECT and JSON_ARRAY
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT JSON_OBJECT('a' VALUE t) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('b' VALUE t) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('a': t) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('a' VALUE t NULL ON NULL) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('a' VALUE t ABSENT ON NULL) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('a' VALUE t WITH UNIQUE KEYS) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('a' VALUE t RETURNING jsonb) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('a' VALUE t FORMAT JSON) FROM pgss_sqljson_tab;
SELECT JSON_ARRAY(t) FROM pgss_sqljson_tab;
SELECT JSON_ARRAY(t ABSENT ON NULL) FROM pgss_sqljson_tab;
SELECT JSON_ARRAY(t NULL ON NULL) FROM pgss_sqljson_tab;
SELECT JSON_ARRAY(t RETURNING jsonb) FROM pgss_sqljson_tab;
SELECT JSON_ARRAY(t FORMAT JSON) FROM pgss_sqljson_tab;
SELECT JSON_ARRAY(SELECT t FROM pgss_sqljson_tab);
SELECT JSON_ARRAY(SELECT t FROM pgss_sqljson_tab FORMAT JSON);
SELECT JSON_ARRAY(SELECT t FROM pgss_sqljson_tab RETURNING jsonb);
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- JSON_OBJECTAGG and JSON_ARRAYAGG
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT JSON_OBJECTAGG(t VALUE t) FROM pgss_sqljson_tab;
SELECT JSON_OBJECTAGG(t VALUE t ABSENT ON NULL) FROM pgss_sqljson_tab;
SELECT JSON_OBJECTAGG(t VALUE t WITH UNIQUE KEYS) FROM pgss_sqljson_tab;
SELECT JSON_OBJECTAGG(t VALUE t RETURNING jsonb) FROM pgss_sqljson_tab;
SELECT JSON_ARRAYAGG(t) FROM pgss_sqljson_tab;
SELECT JSON_ARRAYAGG(t NULL ON NULL) FROM pgss_sqljson_tab;
SELECT JSON_ARRAYAGG(t ORDER BY js) FROM pgss_sqljson_tab;
SELECT JSON_ARRAYAGG(t RETURNING jsonb) FROM pgss_sqljson_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- JSON, JSON_SCALAR, JSON_SERIALIZE and IS JSON
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT JSON(t) FROM pgss_sqljson_tab;
SELECT JSON(t WITH UNIQUE KEYS) FROM pgss_sqljson_tab;
SELECT JSON_SCALAR(t) FROM pgss_sqljson_tab;
SELECT JSON_SERIALIZE(js) FROM pgss_sqljson_tab;
SELECT JSON_SERIALIZE(js RETURNING bytea) FROM pgss_sqljson_tab;
SELECT t IS JSON FROM pgss_sqljson_tab;
SELECT t IS JSON VALUE FROM pgss_sqljson_tab;
SELECT t IS JSON OBJECT FROM pgss_sqljson_tab;
SELECT t IS JSON ARRAY FROM pgss_sqljson_tab;
SELECT t IS JSON SCALAR FROM pgss_sqljson_tab;
SELECT t IS JSON WITH UNIQUE KEYS FROM pgss_sqljson_tab;
SELECT t IS JSON WITHOUT UNIQUE KEYS FROM pgss_sqljson_tab;
SELECT t IS NOT JSON FROM pgss_sqljson_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

DROP TABLE pgss_sqljson_tab;
