/* src/test/modules/test_lossy_gist_ops/test_lossy_gist_ops--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_lossy_gist_ops" to load this file. \quit

CREATE FUNCTION lossy_text_eq_ci(text, text)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR === (
	LEFTARG = text,
	RIGHTARG = text,
	FUNCTION = lossy_text_eq_ci
);

CREATE FUNCTION lossy_text_consistent(internal, text, smallint, oid, internal)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION lossy_text_union(internal, internal)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION lossy_text_penalty(internal, internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION lossy_text_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION lossy_text_same(text, text, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

-- The equality strategy is RTEqualStrategyNumber (18) for GiST.  The
-- translate function is needed so that constraint creation can map equality
-- to the operator class's strategy.
CREATE OPERATOR CLASS lossy_text_ops
	DEFAULT FOR TYPE text USING gist AS
	OPERATOR	18	=== (text, text),
	FUNCTION	1	lossy_text_consistent(internal, text, smallint, oid, internal),
	FUNCTION	2	lossy_text_union(internal, internal),
	FUNCTION	5	lossy_text_penalty(internal, internal, internal),
	FUNCTION	6	lossy_text_picksplit(internal, internal),
	FUNCTION	7	lossy_text_same(text, text, internal),
	FUNCTION	12	("any", "any") gist_translate_cmptype_common(int4);
