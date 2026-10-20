/* contrib/cube/cube--1.5--1.6.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION cube UPDATE TO '1.6'" to load this file. \quit

-- Output depends on extra_float_digits.
ALTER FUNCTION cube_out(cube) STABLE;
