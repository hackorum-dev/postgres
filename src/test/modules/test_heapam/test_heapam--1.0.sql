/* src/test/modules/test_heapam/test_heapam--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_heapam" to load this file. \quit


CREATE FUNCTION heappage_prune_and_freeze(rel regclass, blkno int4)
	RETURNS void
	AS 'MODULE_PATHNAME' LANGUAGE C;


CREATE FUNCTION heappage_craft_new()
	RETURNS void
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION heappage_craft_install(rel regclass, blkno int4)
	RETURNS void
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION heappage_craft_add_tuple(
   offnum       int2,
   xmin         xid,
   xmax         xid,
   cid          cid,
   ctid         tid,
   infoflags    text[],
   data         text
)
	RETURNS void
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION heappage_craft_add_lp_unused(offnum int2)
	RETURNS void
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION heappage_craft_add_lp_redirect(offnum int2, redirect_offnum int2)
	RETURNS void
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION heappage_craft_add_lp_dead(offnum int2)
	RETURNS void
	AS 'MODULE_PATHNAME' LANGUAGE C;
