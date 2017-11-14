/* contrib/pg_buffercache/pg_buffercache--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_buffercache UPDATE TO '1.4'" to load this file. \quit

-- Upgrade view to 1.4. format
CREATE OR REPLACE VIEW pg_buffercache AS
	SELECT P.* FROM pg_buffercache_pages() AS P
	(bufferid integer, relfilenode oid, reltablespace oid, reldatabase oid,
	 relforknumber int2, relblocknumber int8, isdirty bool, usagecount int2,
	 pinning_backends int4, buffer_state int8);

-- Create a function for print of buffer status.
CREATE FUNCTION pg_buffercache_state_print(buffer_state int8)
RETURNS text[] STRICT LANGUAGE 'c'
AS 'MODULE_PATHNAME', 'pg_buffercache_status_print';
