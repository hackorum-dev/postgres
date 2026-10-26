--
-- SQL/XML
--

SET pg_stat_statements.track_utility = FALSE;

CREATE TABLE pgss_xml_tab (x xml, t text);

--
-- SQL/XML functions
--

-- XMLELEMENT, XMLATTRIBUTES, XMLFOREST and XMLPI names
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT XMLELEMENT(NAME foo) FROM pgss_xml_tab;
SELECT XMLELEMENT(NAME bar) FROM pgss_xml_tab;
SELECT XMLELEMENT(NAME foo, t) FROM pgss_xml_tab;
SELECT XMLELEMENT(NAME foo, XMLATTRIBUTES(t AS a)) FROM pgss_xml_tab;
SELECT XMLELEMENT(NAME foo, XMLATTRIBUTES(t AS b)) FROM pgss_xml_tab;
-- the attribute name defaults to the column name
SELECT XMLELEMENT(NAME foo, XMLATTRIBUTES(t)) FROM pgss_xml_tab;
SELECT XMLELEMENT(NAME foo, XMLATTRIBUTES(t AS t)) FROM pgss_xml_tab;
SELECT XMLELEMENT(NAME foo, XMLATTRIBUTES(t AS a), t) FROM pgss_xml_tab;
SELECT XMLFOREST(t AS a) FROM pgss_xml_tab;
SELECT XMLFOREST(t AS b) FROM pgss_xml_tab;
-- the element name defaults to the column name
SELECT XMLFOREST(t) FROM pgss_xml_tab;
SELECT XMLFOREST(t AS t) FROM pgss_xml_tab;
SELECT XMLFOREST(t AS a, x AS b) FROM pgss_xml_tab;
SELECT XMLPI(NAME foo) FROM pgss_xml_tab;
SELECT XMLPI(NAME bar) FROM pgss_xml_tab;
SELECT XMLPI(NAME foo, t) FROM pgss_xml_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- XMLPARSE and XMLSERIALIZE
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT XMLPARSE(DOCUMENT t) FROM pgss_xml_tab;
SELECT XMLPARSE(CONTENT t) FROM pgss_xml_tab;
-- STRIP WHITESPACE is the default
SELECT XMLPARSE(CONTENT t STRIP WHITESPACE) FROM pgss_xml_tab;
SELECT XMLPARSE(CONTENT t PRESERVE WHITESPACE) FROM pgss_xml_tab;
SELECT XMLSERIALIZE(DOCUMENT x AS text) FROM pgss_xml_tab;
SELECT XMLSERIALIZE(CONTENT x AS text) FROM pgss_xml_tab;
-- NO INDENT is the default
SELECT XMLSERIALIZE(CONTENT x AS text NO INDENT) FROM pgss_xml_tab;
SELECT XMLSERIALIZE(CONTENT x AS text INDENT) FROM pgss_xml_tab;
SELECT XMLSERIALIZE(CONTENT x AS varchar(10)) FROM pgss_xml_tab;
SELECT XMLSERIALIZE(CONTENT x AS varchar(20)) FROM pgss_xml_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- XMLROOT
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT XMLROOT(x, VERSION '1.0') FROM pgss_xml_tab;
SELECT XMLROOT(x, VERSION '1.1') FROM pgss_xml_tab;
SELECT XMLROOT(x, VERSION NO VALUE) FROM pgss_xml_tab;
SELECT XMLROOT(x, VERSION '1.0', STANDALONE YES) FROM pgss_xml_tab;
SELECT XMLROOT(x, VERSION '1.0', STANDALONE NO) FROM pgss_xml_tab;
SELECT XMLROOT(x, VERSION '1.0', STANDALONE NO VALUE) FROM pgss_xml_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- XMLCONCAT, IS DOCUMENT and XMLEXISTS
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT XMLCONCAT(x, x) FROM pgss_xml_tab;
SELECT XMLCONCAT(x, x, x) FROM pgss_xml_tab;
SELECT x IS DOCUMENT FROM pgss_xml_tab;
SELECT x IS NOT DOCUMENT FROM pgss_xml_tab;
SELECT XMLEXISTS('/r' PASSING x) FROM pgss_xml_tab;
-- BY REF and BY VALUE are noise words
SELECT XMLEXISTS('/r' PASSING BY REF x) FROM pgss_xml_tab;
SELECT XMLEXISTS('/r' PASSING BY VALUE x BY VALUE) FROM pgss_xml_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

--
-- XMLTABLE
--

-- column types, defaults and NOT NULL
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS a int PATH 'a') xt;
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS a text PATH 'a') xt;
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS a varchar(3) PATH 'a') xt;
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS a varchar(4) PATH 'a') xt;
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS a int PATH 'a' DEFAULT 1) xt;
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS a int PATH 'a' DEFAULT 2) xt;
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS a int PATH 'a' NOT NULL) xt;
-- NULL is the default
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS a int PATH 'a' NULL) xt;
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS a int PATH 'a' DEFAULT 1 NOT NULL) xt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- FOR ORDINALITY, column names and paths
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS a int PATH 'a', b int PATH 'b') xt;
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS a FOR ORDINALITY, b int PATH 'b') xt;
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS a int PATH 'a', b FOR ORDINALITY) xt;
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS o FOR ORDINALITY, a int) xt;
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS a int, o FOR ORDINALITY) xt;
-- only the column names and the paths differ
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS c int PATH 'c', d int PATH 'd') xt;
-- without PATH, the path is the column name
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS a int, b int) xt;
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS c int, d int) xt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- XMLNAMESPACES and PASSING
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING x COLUMNS a int PATH 'a') xt;
SELECT * FROM pgss_xml_tab,
  XMLTABLE(XMLNAMESPACES('http://x' AS n), '/n:r'
    PASSING x COLUMNS a int PATH 'n:a') xt;
-- only the namespace, its name and the paths differ
SELECT * FROM pgss_xml_tab,
  XMLTABLE(XMLNAMESPACES('http://y' AS m), '/m:r'
    PASSING x COLUMNS a int PATH 'm:a') xt;
SELECT * FROM pgss_xml_tab,
  XMLTABLE(XMLNAMESPACES('http://x' AS n, 'http://y' AS m), '/n:r'
    PASSING x COLUMNS a int PATH 'm:a') xt;
-- BY REF and BY VALUE are noise words
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING BY REF x COLUMNS a int PATH 'a') xt;
SELECT * FROM pgss_xml_tab,
  XMLTABLE('/r' PASSING BY VALUE x BY VALUE COLUMNS a int PATH 'a') xt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

DROP TABLE pgss_xml_tab;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
