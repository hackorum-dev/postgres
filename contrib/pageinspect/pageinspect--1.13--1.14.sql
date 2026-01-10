/* contrib/pageinspect/pageinspect--1.13--1.14.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pageinspect UPDATE TO '1.14'" to load this file. \quit

--
-- spgist_page_opaque_info()
--
CREATE FUNCTION spgist_page_opaque_info(IN page bytea,
    OUT lsn pg_lsn,
    OUT nPlaceholder SMALLINT,
    OUT nRedirection SMALLINT,
    OUT flags text[])
AS 'MODULE_PATHNAME', 'spgist_page_opaque_info'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- spgist_leafpage_items()
--
CREATE FUNCTION spgist_leafpage_items(IN page bytea,
    IN index_oid regclass,
    OUT itemoffset smallint,
    OUT ctid tid,
    OUT size smallint,
    OUT hasnullmask boolean,
    OUT nextoffset smallint,
    OUT state TEXT,
    OUT xid xid,
    OUT keys text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'spgist_leafpage_items'
LANGUAGE C STRICT PARALLEL SAFE;


--
-- spgist_innerpage_items()
--
CREATE FUNCTION spgist_innerpage_items(IN page bytea,
    IN index_oid regclass,
    OUT itemoffset smallint,
    OUT allTheSame INT,
    OUT nNodes INT,
    OUT prefixSize INT,
    OUT size smallint,
    OUT state TEXT)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'spgist_innerpage_items'
LANGUAGE C STRICT PARALLEL SAFE;


--
-- spgist_metapage_items()
--
CREATE FUNCTION spgist_metapage_items(IN page bytea,
    OUT itemoffset int,
    OUT blkno smallint,
    OUT freespace int)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'spgist_metapage_items'
LANGUAGE C STRICT PARALLEL SAFE;
