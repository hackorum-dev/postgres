/* contrib/earthdistance/earthdistance--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_stat_statements UPDATE TO '1.2'" to load this file. \quit

-- SQL language function must have search_path from extension schema

ALTER FUNCTION sec_to_gc(float8) SET search_path FROM CURRENT;
ALTER FUNCTION gc_to_sec(float8) SET search_path FROM CURRENT;
ALTER FUNCTION ll_to_earth(float8, float8) SET search_path FROM CURRENT;
ALTER FUNCTION latitude(earth) SET search_path FROM CURRENT;
ALTER FUNCTION longitude(earth) SET search_path FROM CURRENT;
ALTER FUNCTION earth_distance(earth, earth) SET search_path FROM CURRENT;
ALTER FUNCTION earth_box(earth, float8) SET search_path FROM CURRENT;
