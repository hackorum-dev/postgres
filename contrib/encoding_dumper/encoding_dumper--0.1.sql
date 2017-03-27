/* .sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_stat_statements" to load this file. \quit

CREATE FUNCTION encoding_dumper(IN srcenc int,
	   IN	dstenc int,
	   OUT 	srccode bytea,
	   OUT 	dstcode bytea
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'encoding_dumper'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

CREATE FUNCTION encoding_dumper(IN srcencname text,
	   IN	dstencname text,
	   OUT	srccode bytea,
	   OUT	dstcode bytea
)
RETURNS SETOF record AS '
SELECT * FROM encoding_dumper(pg_char_to_encoding(srcencname),
							  pg_char_to_encoding(dstencname));
' LANGUAGE SQL;
