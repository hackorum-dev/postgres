/* contrib/cube/cube--1.5--1.6.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION cube UPDATE TO '1.6'" to load this file. \quit

CREATE FUNCTION cube_add(cube, cube)
RETURNS cube
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION cube_sub(cube, cube)
RETURNS cube
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION cube_mul_cf(cube, float8)
RETURNS cube
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION cube_mul_fc(float8, cube)
RETURNS cube
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION cube_div(cube, float8)
RETURNS cube
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Add coordinate-wise binary operators

CREATE OPERATOR + (
	LEFTARG = cube, RIGHTARG = cube, PROCEDURE = cube_add,
	COMMUTATOR = '+'
);

CREATE OPERATOR - (
	LEFTARG = cube, RIGHTARG = cube, PROCEDURE = cube_sub
);

-- Add coordinate-wise binary operators with scalars

CREATE OPERATOR / (
	LEFTARG = cube, RIGHTARG = float8, PROCEDURE = cube_div
);

CREATE OPERATOR * (
	LEFTARG = cube, RIGHTARG = float8, PROCEDURE = cube_mul_cf,
	COMMUTATOR = '*'
);

CREATE OPERATOR * (
	LEFTARG = float8, RIGHTARG = cube, PROCEDURE = cube_mul_fc,
	COMMUTATOR = '*'
);
