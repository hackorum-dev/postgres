/* contrib/anyarray/anyarray--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION anyarray" to load this file. \quit

--
-- Set-style operations for arrays of any btree-orderable element type.
--

CREATE FUNCTION anyarray_sort(anyarray)
RETURNS anyarray
AS 'MODULE_PATHNAME', 'anyarray_sort'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION anyarray_sort(anyarray, text)
RETURNS anyarray
AS 'MODULE_PATHNAME', 'anyarray_sort_dir'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION anyarray_uniq(anyarray)
RETURNS anyarray
AS 'MODULE_PATHNAME', 'anyarray_uniq'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION anyarray_idx(anyarray, anyelement)
RETURNS int4
AS 'MODULE_PATHNAME', 'anyarray_idx'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION anyarray_subarray(anyarray, int4, int4)
RETURNS anyarray
AS 'MODULE_PATHNAME', 'anyarray_subarray'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION anyarray_subarray(anyarray, int4)
RETURNS anyarray
AS 'MODULE_PATHNAME', 'anyarray_subarray_to_end'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION anyarray_icount(anyarray)
RETURNS int4
AS 'MODULE_PATHNAME', 'anyarray_icount'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION anyarray_intersect(anyarray, anyarray)
RETURNS anyarray
AS 'MODULE_PATHNAME', 'anyarray_intersect'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION anyarray_union(anyarray, anyarray)
RETURNS anyarray
AS 'MODULE_PATHNAME', 'anyarray_union'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION anyarray_union_elem(anyarray, anyelement)
RETURNS anyarray
AS 'MODULE_PATHNAME', 'anyarray_union_elem'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION anyarray_difference(anyarray, anyarray)
RETURNS anyarray
AS 'MODULE_PATHNAME', 'anyarray_difference'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION anyarray_difference_elem(anyarray, anyelement)
RETURNS anyarray
AS 'MODULE_PATHNAME', 'anyarray_difference_elem'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

--
-- Operators
--

-- Unary prefix # : cardinality (number of elements)
CREATE OPERATOR # (
	RIGHTARG = anyarray,
	PROCEDURE = anyarray_icount
);

-- array # element : 1-based index of element, or 0 if not found
CREATE OPERATOR # (
	LEFTARG = anyarray,
	RIGHTARG = anyelement,
	PROCEDURE = anyarray_idx
);

-- Set intersection
CREATE OPERATOR & (
	LEFTARG = anyarray,
	RIGHTARG = anyarray,
	COMMUTATOR = &,
	PROCEDURE = anyarray_intersect
);

-- Set union (array | array)
CREATE OPERATOR | (
	LEFTARG = anyarray,
	RIGHTARG = anyarray,
	COMMUTATOR = |,
	PROCEDURE = anyarray_union
);

-- Append element (array | element)
CREATE OPERATOR | (
	LEFTARG = anyarray,
	RIGHTARG = anyelement,
	PROCEDURE = anyarray_union_elem
);

-- Set difference (array - array)
CREATE OPERATOR - (
	LEFTARG = anyarray,
	RIGHTARG = anyarray,
	PROCEDURE = anyarray_difference
);

-- Remove all occurrences of element (array - element)
CREATE OPERATOR - (
	LEFTARG = anyarray,
	RIGHTARG = anyelement,
	PROCEDURE = anyarray_difference_elem
);

--
-- Boolean query type
--

CREATE FUNCTION anyquery_in(cstring)
RETURNS anyquery
AS 'MODULE_PATHNAME', 'anyquery_in'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION anyquery_out(anyquery)
RETURNS cstring
AS 'MODULE_PATHNAME', 'anyquery_out'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE TYPE anyquery (
	INTERNALLENGTH = -1,
	INPUT = anyquery_in,
	OUTPUT = anyquery_out,
	STORAGE = extended
);

CREATE FUNCTION anyquery_querytree(anyquery)
RETURNS text
AS 'MODULE_PATHNAME', 'anyquery_querytree'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION anyarray_boolop(anyarray, anyquery)
RETURNS bool
AS 'MODULE_PATHNAME', 'anyarray_boolop'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION anyquery_boolop_rev(anyquery, anyarray)
RETURNS bool
AS 'MODULE_PATHNAME', 'anyquery_boolop_rev'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE OPERATOR @@ (
	LEFTARG = anyarray,
	RIGHTARG = anyquery,
	PROCEDURE = anyarray_boolop,
	COMMUTATOR = '~~',
	RESTRICT = contsel,
	JOIN = contjoinsel
);

CREATE OPERATOR ~~ (
	LEFTARG = anyquery,
	RIGHTARG = anyarray,
	PROCEDURE = anyquery_boolop_rev,
	COMMUTATOR = '@@',
	RESTRICT = contsel,
	JOIN = contjoinsel
);

--
-- GiST opclass (signature-based)
--

CREATE FUNCTION anyarray_gist_key_in(cstring)
RETURNS anyarray_gist_key
AS 'MODULE_PATHNAME', 'anyarray_gist_key_in'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION anyarray_gist_key_out(anyarray_gist_key)
RETURNS cstring
AS 'MODULE_PATHNAME', 'anyarray_gist_key_out'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE TYPE anyarray_gist_key (
	INTERNALLENGTH = -1,
	INPUT = anyarray_gist_key_in,
	OUTPUT = anyarray_gist_key_out
);

CREATE FUNCTION anyarray_gist_consistent(internal, anyarray, smallint, oid, internal)
RETURNS bool
AS 'MODULE_PATHNAME', 'anyarray_gist_consistent'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION anyarray_gist_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'anyarray_gist_compress'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION anyarray_gist_decompress(internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'anyarray_gist_decompress'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION anyarray_gist_union(internal, internal)
RETURNS anyarray_gist_key
AS 'MODULE_PATHNAME', 'anyarray_gist_union'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION anyarray_gist_same(anyarray_gist_key, anyarray_gist_key, internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'anyarray_gist_same'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION anyarray_gist_penalty(internal, internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'anyarray_gist_penalty'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION anyarray_gist_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'anyarray_gist_picksplit'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION anyarray_gist_options(internal)
RETURNS void
AS 'MODULE_PATHNAME', 'anyarray_gist_options'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR CLASS anyarray_gist_ops
FOR TYPE anyarray USING gist
AS
	OPERATOR	3	&&,
	OPERATOR	7	@>,
	OPERATOR	8	<@,
	OPERATOR	18	=,
	OPERATOR	20	@@ (anyarray, anyquery),
	FUNCTION	1	anyarray_gist_consistent(internal, anyarray, smallint, oid, internal),
	FUNCTION	2	anyarray_gist_union(internal, internal),
	FUNCTION	3	anyarray_gist_compress(internal),
	FUNCTION	4	anyarray_gist_decompress(internal),
	FUNCTION	5	anyarray_gist_penalty(internal, internal, internal),
	FUNCTION	6	anyarray_gist_picksplit(internal, internal),
	FUNCTION	7	anyarray_gist_same(anyarray_gist_key, anyarray_gist_key, internal),
	FUNCTION	10	anyarray_gist_options(internal),
	STORAGE		anyarray_gist_key;

--
-- GIN opclasses (per concrete element type, supporting standard ops + @@)
--

CREATE FUNCTION anyarray_gin_extract_query_int8(
	int8[], internal, smallint, internal, internal, internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'anyarray_gin_extract_query_int8'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION anyarray_gin_consistent_int8(
	internal, smallint, int8[], integer, internal, internal, internal, internal)
RETURNS bool
AS 'MODULE_PATHNAME', 'anyarray_gin_consistent_int8'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR CLASS int8_anyquery_gin_ops
FOR TYPE int8[] USING gin
AS
	OPERATOR	1	&& (anyarray, anyarray),
	OPERATOR	2	@> (anyarray, anyarray),
	OPERATOR	3	<@ (anyarray, anyarray),
	OPERATOR	4	= (anyarray, anyarray),
	OPERATOR	5	@@ (anyarray, anyquery),
	FUNCTION	1	btint8cmp(int8, int8),
	FUNCTION	2	ginarrayextract(anyarray, internal, internal),
	FUNCTION	3	anyarray_gin_extract_query_int8(int8[], internal, smallint, internal, internal, internal, internal),
	FUNCTION	4	anyarray_gin_consistent_int8(internal, smallint, int8[], integer, internal, internal, internal, internal),
	STORAGE		int8;

CREATE FUNCTION anyarray_gin_extract_query_uuid(
	uuid[], internal, smallint, internal, internal, internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'anyarray_gin_extract_query_uuid'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION anyarray_gin_consistent_uuid(
	internal, smallint, uuid[], integer, internal, internal, internal, internal)
RETURNS bool
AS 'MODULE_PATHNAME', 'anyarray_gin_consistent_uuid'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR CLASS uuid_anyquery_gin_ops
FOR TYPE uuid[] USING gin
AS
	OPERATOR	1	&& (anyarray, anyarray),
	OPERATOR	2	@> (anyarray, anyarray),
	OPERATOR	3	<@ (anyarray, anyarray),
	OPERATOR	4	= (anyarray, anyarray),
	OPERATOR	5	@@ (anyarray, anyquery),
	FUNCTION	1	uuid_cmp(uuid, uuid),
	FUNCTION	2	ginarrayextract(anyarray, internal, internal),
	FUNCTION	3	anyarray_gin_extract_query_uuid(uuid[], internal, smallint, internal, internal, internal, internal),
	FUNCTION	4	anyarray_gin_consistent_uuid(internal, smallint, uuid[], integer, internal, internal, internal, internal),
	STORAGE		uuid;

CREATE FUNCTION anyarray_gin_extract_query_text(
	text[], internal, smallint, internal, internal, internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'anyarray_gin_extract_query_text'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION anyarray_gin_consistent_text(
	internal, smallint, text[], integer, internal, internal, internal, internal)
RETURNS bool
AS 'MODULE_PATHNAME', 'anyarray_gin_consistent_text'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR CLASS text_anyquery_gin_ops
FOR TYPE text[] USING gin
AS
	OPERATOR	1	&& (anyarray, anyarray),
	OPERATOR	2	@> (anyarray, anyarray),
	OPERATOR	3	<@ (anyarray, anyarray),
	OPERATOR	4	= (anyarray, anyarray),
	OPERATOR	5	@@ (anyarray, anyquery),
	FUNCTION	1	bttextcmp(text, text),
	FUNCTION	2	ginarrayextract(anyarray, internal, internal),
	FUNCTION	3	anyarray_gin_extract_query_text(text[], internal, smallint, internal, internal, internal, internal),
	FUNCTION	4	anyarray_gin_consistent_text(internal, smallint, text[], integer, internal, internal, internal, internal),
	STORAGE		text;
