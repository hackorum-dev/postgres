--
-- Setup
--

-- Databases and roles to test filtering by their oids
CREATE DATABASE regression_db1;
CREATE DATABASE regression_db2;
CREATE ROLE regress_user1 LOGIN SUPERUSER;
CREATE ROLE regress_user2 LOGIN SUPERUSER;

SELECT oid AS db1_oid FROM pg_database WHERE datname = 'regression_db1' \gset
SELECT oid AS db2_oid FROM pg_database WHERE datname = 'regression_db2' \gset
SELECT oid AS user1_oid FROM pg_authid WHERE rolname = 'regress_user1' \gset
SELECT oid AS user2_oid FROM pg_authid WHERE rolname = 'regress_user2' \gset

-- Role to run all other queries
CREATE ROLE regress_user LOGIN SUPERUSER;
SET ROLE regress_user;

-- Reset statistics to start clean
SELECT pg_stat_statements_reset() IS NOT NULL AS t;


--
-- Run all test queries
--

-- db1, user1
\c regression_db1
SET ROLE regress_user1;
SELECT 'multiple_query_text' as multiple_test;
SELECT 'text_A'::text, 1;

-- db1, user2
SET ROLE regress_user2;
SELECT 'multiple_query_text' as multiple_test;
SELECT 'text_B'::text, 1, 2;

-- db2, user1
\c regression_db2
SET ROLE regress_user1;
SELECT 'multiple_query_text' as multiple_test;
SELECT 'text_C'::text, 1, 2, 3;

-- db2, user2
SET ROLE regress_user2;
SELECT 'multiple_query_text' as multiple_test;
SELECT 'text_D'::text, 1, 2, 3, 4;

-- Switch to db and user other then db1, db2, user1, user2 to run tests
\c contrib_regression
SET ROLE regress_user;

--
-- Test 1: All zeroes (default values) should returns all records (no filtering)
--

SELECT rolname, datname, query, calls, rows
FROM pg_stat_statements(true) pgss
JOIN pg_roles ON (pgss.userid = pg_roles.oid)
JOIN pg_database ON (pgss.dbid = pg_database.oid)
ORDER BY rolname, datname, query COLLATE "C";

SELECT rolname, datname, query, calls, rows
FROM pg_stat_statements(true, 0, 0, 0) pgss
JOIN pg_roles ON (pgss.userid = pg_roles.oid)
JOIN pg_database ON (pgss.dbid = pg_database.oid)
ORDER BY rolname, datname, query COLLATE "C";

--
-- Test 2: Filter by userid only
--

SELECT rolname, datname, query, calls, rows
FROM pg_stat_statements(true, :user1_oid, 0, 0) pgss
JOIN pg_roles ON (pgss.userid = pg_roles.oid)
JOIN pg_database ON (pgss.dbid = pg_database.oid)
ORDER BY rolname, datname, query COLLATE "C";

SELECT rolname, datname, query, calls, rows
FROM pg_stat_statements(true, :user2_oid, 0, 0) pgss
JOIN pg_roles ON (pgss.userid = pg_roles.oid)
JOIN pg_database ON (pgss.dbid = pg_database.oid)
ORDER BY rolname, datname, query COLLATE "C";

--
-- Test 3: Filter by dbid only
--

SELECT rolname, datname, query, calls, rows
FROM pg_stat_statements(true, 0, :db1_oid, 0) pgss
JOIN pg_roles ON (pgss.userid = pg_roles.oid)
JOIN pg_database ON (pgss.dbid = pg_database.oid)
ORDER BY rolname, datname, query COLLATE "C";

SELECT rolname, datname, query, calls, rows
FROM pg_stat_statements(true, 0, :db2_oid, 0) pgss
JOIN pg_roles ON (pgss.userid = pg_roles.oid)
JOIN pg_database ON (pgss.dbid = pg_database.oid)
ORDER BY rolname, datname, query COLLATE "C";


--
-- Get query IDs
--

SELECT queryid AS query1_id FROM pg_stat_statements
WHERE query = 'SELECT $1::text, $2' \gset

SELECT queryid AS query2_id FROM pg_stat_statements
WHERE query = 'SELECT $1::text, $2, $3, $4, $5' \gset

SELECT queryid AS multiple_query_id FROM pg_stat_statements
WHERE query = 'SELECT $1 as multiple_test' LIMIT 1 \gset

--
-- Test 4: Filter by queryid only
--

SELECT rolname, datname, query, calls, rows
FROM pg_stat_statements(true, 0, 0, :query1_id) pgss
JOIN pg_roles ON (pgss.userid = pg_roles.oid)
JOIN pg_database ON (pgss.dbid = pg_database.oid)
ORDER BY rolname, datname, query COLLATE "C";

SELECT rolname, datname, query, calls, rows
FROM pg_stat_statements(true, 0, 0, :query2_id) pgss
JOIN pg_roles ON (pgss.userid = pg_roles.oid)
JOIN pg_database ON (pgss.dbid = pg_database.oid)
ORDER BY rolname, datname, query COLLATE "C";

SELECT rolname, datname, query, calls, rows
FROM pg_stat_statements(true, 0, 0, :multiple_query_id) pgss
JOIN pg_roles ON (pgss.userid = pg_roles.oid)
JOIN pg_database ON (pgss.dbid = pg_database.oid)
ORDER BY rolname, datname, query COLLATE "C";

--
-- Test 5: Filter by userid and dbid
--

SELECT rolname, datname, query, calls, rows
FROM pg_stat_statements(true, :user1_oid, :db2_oid, 0) pgss
JOIN pg_roles ON (pgss.userid = pg_roles.oid)
JOIN pg_database ON (pgss.dbid = pg_database.oid)
ORDER BY rolname, datname, query COLLATE "C";

--
-- Test 6: Filter by userid and queryid
--

SELECT rolname, datname, query, calls, rows
FROM pg_stat_statements(true, :user2_oid, 0, :multiple_query_id) pgss
JOIN pg_roles ON (pgss.userid = pg_roles.oid)
JOIN pg_database ON (pgss.dbid = pg_database.oid)
ORDER BY rolname, datname, query COLLATE "C";

--
-- Test 7: Filter by dbid and queryid
--

SELECT rolname, datname, query, calls, rows
FROM pg_stat_statements(true, 0, :db1_oid, :multiple_query_id) pgss
JOIN pg_roles ON (pgss.userid = pg_roles.oid)
JOIN pg_database ON (pgss.dbid = pg_database.oid)
ORDER BY rolname, datname, query COLLATE "C";


--
-- Test 8: Filter by userid and dbid and queryid
--

SELECT rolname, datname, query, calls, rows
FROM pg_stat_statements(true, :user1_oid, :db1_oid, :query1_id) pgss
JOIN pg_roles ON (pgss.userid = pg_roles.oid)
JOIN pg_database ON (pgss.dbid = pg_database.oid)
ORDER BY rolname, datname, query COLLATE "C";

SELECT rolname, datname, query, calls, rows
FROM pg_stat_statements(true, :user2_oid, :db1_oid, :multiple_query_id) pgss
JOIN pg_roles ON (pgss.userid = pg_roles.oid)
JOIN pg_database ON (pgss.dbid = pg_database.oid)
ORDER BY rolname, datname, query COLLATE "C";

--
-- Test 9: No matching queries
--

SELECT rolname, datname, query, calls, rows
FROM pg_stat_statements(true, :user2_oid, 0, :query1_id) pgss
JOIN pg_roles ON (pgss.userid = pg_roles.oid)
JOIN pg_database ON (pgss.dbid = pg_database.oid)
ORDER BY rolname, datname, query COLLATE "C";

SELECT rolname, datname, query, calls, rows
FROM pg_stat_statements(true, :user1_oid, :db1_oid, :query2_id) pgss
JOIN pg_roles ON (pgss.userid = pg_roles.oid)
JOIN pg_database ON (pgss.dbid = pg_database.oid)
ORDER BY rolname, datname, query COLLATE "C";


--
-- Cleanup
--

DROP DATABASE regression_db1;
DROP DATABASE regression_db2;

RESET ROLE;
DROP ROLE regress_user1;
DROP ROLE regress_user2;
DROP ROLE regress_user;

SELECT pg_stat_statements_reset() IS NOT NULL AS t;
