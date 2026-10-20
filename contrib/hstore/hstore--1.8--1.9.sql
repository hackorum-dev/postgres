/* contrib/hstore/hstore--1.8--1.9.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION hstore UPDATE TO '1.9'" to load this file. \quit

-- These call the output or input functions of the record's column types,
-- whose results can depend on settings such as TimeZone.
ALTER FUNCTION hstore(record) STABLE;
ALTER FUNCTION populate_record(anyelement, hstore) STABLE;
