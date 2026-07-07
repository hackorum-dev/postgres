--
-- Verify that constructs which resolve an operator by unqualified name
-- (IS [NOT] DISTINCT FROM, NULLIF, ...) deparse with a schema-qualified
-- OPERATOR() decoration when the operator is not reachable via search_path,
-- and that the decorated syntax parses back to the same semantics.
--
CREATE SCHEMA alt_ops;
-- A shadow "=" over int4: same semantics as pg_catalog's (int4eq), but a
-- distinct OID, so resolution differences are observable without contrib.
CREATE OPERATOR alt_ops.= (
    leftarg = int4, rightarg = int4, procedure = int4eq,
    commutator = operator(alt_ops.=), restrict = eqsel, join = eqjoinsel,
    hashes, merges
);
-- alt_ops.= declares "hashes", so back the declaration up: give the operator
-- the hash opfamily membership the planner will look for whenever it builds
-- a hash table with it (e.g. for a hashed subplan).
CREATE OPERATOR CLASS alt_ops.int4_alt_hash_ops FOR TYPE int4 USING hash AS
    OPERATOR 1 alt_ops.=,
    FUNCTION 1 hashint4(int4);
CREATE TABLE oq_t (id int, a int, b int);

-- Views created while alt_ops.= shadows pg_catalog.=
SET search_path = alt_ops, pg_catalog;
CREATE VIEW public.oq_v_distinct AS
  SELECT id, a IS DISTINCT FROM b AS r FROM public.oq_t;
CREATE VIEW public.oq_v_notdistinct AS
  SELECT id, a IS NOT DISTINCT FROM b AS r FROM public.oq_t;
CREATE VIEW public.oq_v_nullif AS
  SELECT id, NULLIF(a, b) AS r FROM public.oq_t;
RESET search_path;

-- With alt_ops off the search_path, the operator must be qualified.
SELECT pg_get_viewdef('oq_v_distinct'::regclass, true);
SELECT pg_get_viewdef('oq_v_notdistinct'::regclass, true);
SELECT pg_get_viewdef('oq_v_nullif'::regclass, true);

-- With alt_ops reachable again, the plain syntax comes back.
SET search_path = alt_ops, pg_catalog, public;
SELECT pg_get_viewdef('oq_v_distinct'::regclass, true);
SELECT pg_get_viewdef('oq_v_nullif'::regclass, true);
RESET search_path;

-- A bare-resolvable operator whose name is not "=" must still be decorated:
-- reparsing the undecorated construct would resolve "=", not this operator.
-- pg_catalog.< is on the default search_path, so its name prints unqualified,
-- but it is wrapped in OPERATOR() so the reparsed semantics stay identical.
CREATE VIEW public.oq_v_distinct_lt AS SELECT 1 IS DISTINCT OPERATOR(<) FROM 2 AS r;
CREATE VIEW public.oq_v_nullif_lt AS SELECT NULLIF(1, 2 USING OPERATOR(<)) AS r;
SELECT pg_get_viewdef('oq_v_distinct_lt'::regclass, true);
SELECT pg_get_viewdef('oq_v_nullif_lt'::regclass, true);

-- The decorated syntax must be directly acceptable (a_expr and b_expr).
SELECT 1 IS DISTINCT OPERATOR(alt_ops.=) FROM 2 AS t;
SELECT 1 IS NOT DISTINCT OPERATOR(alt_ops.=) FROM 2 AS f;
SELECT NULLIF(1, 2 USING OPERATOR(alt_ops.=)) AS one;
SELECT NULLIF(2, 2 USING OPERATOR(alt_ops.=)) AS n;
-- b_expr context (typmod-less contexts use b_expr)
CREATE TABLE oq_bexpr_check (x bool DEFAULT 1 IS DISTINCT OPERATOR(pg_catalog.=) FROM 2);
DROP TABLE oq_bexpr_check;
-- unqualified name inside the decoration is fine too
SELECT 1 IS DISTINCT OPERATOR(=) FROM 2 AS t;

-- Semantics identical to the plain construct, including NULL handling.
SELECT x IS DISTINCT OPERATOR(alt_ops.=) FROM y AS dist,
       NULLIF(x, y USING OPERATOR(alt_ops.=)) AS nif
FROM (VALUES (1, 1), (1, 2), (NULL::int, 1), (NULL::int, NULL::int)) v(x, y);

-- A non-boolean operator is rejected, same as parse analysis rejects today.
CREATE OPERATOR alt_ops.+ (leftarg = int4, rightarg = int4, procedure = int4pl);
SELECT NULLIF(1, 2 USING OPERATOR(alt_ops.+));
SELECT 1 IS DISTINCT OPERATOR(alt_ops.+) FROM 2;

-- When one side is the NULL literal, the construct degenerates to a
-- NullTest and the given operator is (deliberately) not consulted.
SELECT 1 IS DISTINCT OPERATOR(alt_ops.=) FROM NULL AS t;

-- The deparsed definitions must reparse under a restricted search_path
-- (this is what pg_dump does; it failed before this patch).
BEGIN;
SET LOCAL search_path = pg_catalog;
DO $$
BEGIN
  EXECUTE 'CREATE VIEW public.oq_v_distinct_reload AS '
          || pg_get_viewdef('public.oq_v_distinct'::regclass);
  EXECUTE 'CREATE VIEW public.oq_v_notdistinct_reload AS '
          || pg_get_viewdef('public.oq_v_notdistinct'::regclass);
  EXECUTE 'CREATE VIEW public.oq_v_nullif_reload AS '
          || pg_get_viewdef('public.oq_v_nullif'::regclass);
  EXECUTE 'CREATE VIEW public.oq_v_distinct_lt_reload AS '
          || pg_get_viewdef('public.oq_v_distinct_lt'::regclass);
  EXECUTE 'CREATE VIEW public.oq_v_nullif_lt_reload AS '
          || pg_get_viewdef('public.oq_v_nullif_lt'::regclass);
END $$;
COMMIT;
-- Reloaded views resolve the same operator OIDs as the originals.
SELECT ev_class::regclass FROM pg_rewrite
WHERE ev_action LIKE '%:opno ' || 'alt_ops.=(int4,int4)'::regoperator::oid || ' %'
  AND ev_class::regclass::text LIKE 'oq_v%reload'
ORDER BY 1;
-- A bare-but-non-"=" operator (here pg_catalog.<) survives the round-trip too:
-- the OPERATOR(<) decoration pins int4lt instead of letting "=" resolve.
SELECT ev_class::regclass FROM pg_rewrite
WHERE ev_action LIKE '%:opno ' || 'pg_catalog.<(int4,int4)'::regoperator::oid || ' %'
  AND ev_class::regclass::text LIKE 'oq_v%lt_reload%'
ORDER BY 1;
DROP VIEW oq_v_distinct_reload, oq_v_notdistinct_reload, oq_v_nullif_reload,
          oq_v_distinct_lt_reload, oq_v_nullif_lt_reload;

--
-- Trigger WHEN clauses deparse through the same code path
-- (pg_get_triggerdef).  This is the dominant real-world shape of the
-- problem: a BEFORE UPDATE trigger whose WHEN test uses IS DISTINCT FROM on
-- a column whose "=" is off the search_path used at restore time.
--
CREATE FUNCTION oq_trgfn() RETURNS trigger LANGUAGE plpgsql
  AS $$ BEGIN RETURN NEW; END $$;
SET search_path = alt_ops, pg_catalog;
CREATE TRIGGER oq_trg BEFORE UPDATE ON public.oq_t FOR EACH ROW
  WHEN (OLD.a IS DISTINCT FROM NEW.a) EXECUTE FUNCTION public.oq_trgfn();
RESET search_path;
-- With alt_ops off the search_path the WHEN operator must be qualified.
SELECT pg_get_triggerdef(oid, true) FROM pg_trigger WHERE tgname = 'oq_trg';
-- The deparsed definition must recreate the trigger under a restricted
-- search_path (as pg_dump does at restore time).  alt_ops is deliberately
-- absent below, so the OPERATOR(alt_ops.=) decoration in the WHEN clause is
-- what pins the correct operator; public is present only so the trigger's
-- table and function resolve.  Capture the definition before dropping.
BEGIN;
SET LOCAL search_path = public, pg_catalog;
DO $$
DECLARE def text;
BEGIN
  SELECT pg_get_triggerdef(oid) INTO def FROM pg_trigger
    WHERE tgname = 'oq_trg' AND tgrelid = 'public.oq_t'::regclass;
  EXECUTE 'DROP TRIGGER oq_trg ON public.oq_t';
  EXECUTE def;
END $$;
COMMIT;
-- The reloaded trigger's WHEN condition resolves the shadow alt_ops.= (its
-- OID appears in tgqual), not pg_catalog.=.
SELECT tgname FROM pg_trigger
WHERE tgqual LIKE '%:opno ' || 'alt_ops.=(int4,int4)'::regoperator::oid || ' %'
  AND tgname = 'oq_trg';
-- NOTE: oq_t, the oq_v_* views, and the oq_trg trigger (backed by oq_trgfn)
-- are intentionally kept (not dropped): the pg_upgrade test suite
-- dump/restores the regression database and thereby exercises the qualified
-- deparse end-to-end.

-- JOIN USING with an off-path equality operator
CREATE TABLE oq_t2 (id int, a int);
SET search_path = alt_ops, pg_catalog;
CREATE VIEW public.oq_v_using AS
  SELECT t.id FROM public.oq_t t JOIN public.oq_t2 u USING (id, a);
CREATE VIEW public.oq_v_natural AS
  SELECT t.id FROM public.oq_t t NATURAL JOIN public.oq_t2 u;
-- Mixed list: pin id's operator to pg_catalog.= and a's to the shadow
-- alt_ops.=.  Under the default path the former resolves bare while the
-- latter must be qualified, so the emitted list is "OPERATOR (=, alt_ops.=)".
CREATE VIEW public.oq_v_using_mixed AS
  SELECT t.id FROM public.oq_t t JOIN public.oq_t2 u
    USING (id, a) OPERATOR (pg_catalog.=, alt_ops.=);
RESET search_path;
-- A join operator whose name is not "=" (here pg_catalog.<, on the default
-- path) must still force the list: a bare USING clause would reparse as "=".
CREATE VIEW public.oq_v_using_lt AS
  SELECT t.id FROM public.oq_t t JOIN public.oq_t2 u USING (id) OPERATOR (<);
SELECT pg_get_viewdef('oq_v_using'::regclass, true);
SELECT pg_get_viewdef('oq_v_natural'::regclass, true);
SELECT pg_get_viewdef('oq_v_using_mixed'::regclass, true);
SELECT pg_get_viewdef('oq_v_using_lt'::regclass, true);
-- explicit syntax parses; mixed bare/qualified names allowed
SELECT t.id FROM oq_t t JOIN oq_t2 u USING (id, a) OPERATOR (pg_catalog.=, alt_ops.=) WHERE false;
SELECT t.id FROM oq_t t JOIN oq_t2 u USING (id, a) OPERATOR (=, alt_ops.=) AS j WHERE false;
-- arity mismatch is an error
SELECT t.id FROM oq_t t JOIN oq_t2 u USING (id, a) OPERATOR (alt_ops.=) WHERE false;
-- reload round-trip under restricted search_path
BEGIN; SET LOCAL search_path = pg_catalog;
DO $$ BEGIN
  EXECUTE 'CREATE VIEW public.oq_v_using_reload AS ' || pg_get_viewdef('public.oq_v_using'::regclass);
  EXECUTE 'CREATE VIEW public.oq_v_natural_reload AS ' || pg_get_viewdef('public.oq_v_natural'::regclass);
  EXECUTE 'CREATE VIEW public.oq_v_using_mixed_reload AS ' || pg_get_viewdef('public.oq_v_using_mixed'::regclass);
  EXECUTE 'CREATE VIEW public.oq_v_using_lt_reload AS ' || pg_get_viewdef('public.oq_v_using_lt'::regclass);
END $$; COMMIT;
-- Reloaded views resolve the same operator OIDs as the originals: alt_ops.=
-- for oq_v_using/natural (both columns) and the mixed view (column a only).
SELECT ev_class::regclass FROM pg_rewrite
WHERE ev_action LIKE '%:opno ' || 'alt_ops.=(int4,int4)'::regoperator::oid || ' %'
  AND ev_class::regclass::text LIKE 'oq_v%reload'
ORDER BY 1;
-- The non-"=" operator OPERATOR(<) pins int4lt across the reload.
SELECT ev_class::regclass FROM pg_rewrite
WHERE ev_action LIKE '%:opno ' || 'pg_catalog.<(int4,int4)'::regoperator::oid || ' %'
  AND ev_class::regclass::text LIKE 'oq_v%reload'
ORDER BY 1;
DROP VIEW oq_v_using_reload, oq_v_natural_reload, oq_v_using_mixed_reload,
          oq_v_using_lt_reload;

--
-- Row-comparison operator lists: the OPERATOR() decoration may carry a
-- comma-separated LIST of operators, one per column of the compared rows.
--
-- make_row_comparison_op intersects the per-column operators' btree opfamily
-- interpretations, so a bare "<" with no operator class fails outright with
-- "could not determine interpretation of row comparison operator".  A btree
-- operator class over int4 is therefore required.  alt_ops.= already exists
-- above; add the rest of the strategy set and wrap them in a class.
CREATE OPERATOR alt_ops.< (leftarg = int4, rightarg = int4, procedure = int4lt);
CREATE OPERATOR alt_ops.<= (leftarg = int4, rightarg = int4, procedure = int4le);
CREATE OPERATOR alt_ops.> (leftarg = int4, rightarg = int4, procedure = int4gt);
CREATE OPERATOR alt_ops.>= (leftarg = int4, rightarg = int4, procedure = int4ge);
CREATE OPERATOR CLASS alt_ops.int4_alt_ops FOR TYPE int4 USING btree AS
    OPERATOR 1 alt_ops.<,
    OPERATOR 2 alt_ops.<=,
    OPERATOR 3 alt_ops.=,
    OPERATOR 4 alt_ops.>=,
    OPERATOR 5 alt_ops.>,
    FUNCTION 1 btint4cmp(int4, int4);

-- View built while alt_ops shadows pg_catalog: the row comparison resolves to
-- alt_ops.< for both columns.
SET search_path = alt_ops, pg_catalog;
CREATE VIEW public.oq_v_rowcmp AS
  SELECT ROW(a, b) < ROW(b, a) AS r FROM public.oq_t;
RESET search_path;
-- Off the path, both column operators must be qualified: the decoration is a
-- LIST, one operator per column (ROW(...) OPERATOR(alt_ops.<, alt_ops.<) ROW(...)).
SELECT pg_get_viewdef('oq_v_rowcmp'::regclass, true);
-- With alt_ops reachable again, the plain single-operator syntax comes back.
SET search_path = alt_ops, pg_catalog, public;
SELECT pg_get_viewdef('oq_v_rowcmp'::regclass, true);
RESET search_path;

-- Mixed-schema deparse: build (under the default search_path) a view whose row
-- comparison pins column 1 to alt_ops.< (off the path -> must be qualified) and
-- column 2 to pg_catalog.< (reachable bare).  A single written name cannot
-- recover two differently-spelled operators, so the deparse emits the per-column
-- list OPERATOR(alt_ops.<, <): only column 1 is qualified, but the list form is
-- forced because the two column names differ.
CREATE VIEW public.oq_v_rowcmp_mixed AS
  SELECT ROW(a, b) OPERATOR(alt_ops.<, pg_catalog.<) ROW(b, a) AS r
  FROM public.oq_t;
SELECT pg_get_viewdef('oq_v_rowcmp_mixed'::regclass, true);

-- The list syntax is directly acceptable and N operators compare N columns.
SELECT ROW(1,2) OPERATOR(pg_catalog.<, pg_catalog.<) ROW(2,1) AS t;
-- Mixed-schema list: column 1 pinned to alt_ops.<, column 2 to pg_catalog.<.
SELECT ROW(1,2) OPERATOR(alt_ops.<, pg_catalog.<) ROW(2,3) AS t;
-- A one-element list is exactly today's single-operator syntax (invariance
-- guard: this line is already accepted before the list grammar lands).
SELECT ROW(1,2) OPERATOR(pg_catalog.<) ROW(1,3) AS t;
-- A mixed-schema equality list is accepted; both columns compare equal (t).
SELECT ROW(1,2) OPERATOR(pg_catalog.=, alt_ops.=) ROW(1,2) AS t;

-- Errors below run under terse verbosity: one line per error, the message
-- text plus "at character N" pinning the OPERATOR() decoration's position.
\set VERBOSITY terse
-- Too many operators for the row width.
SELECT ROW(1,2) OPERATOR(pg_catalog.<, pg_catalog.<, pg_catalog.<) ROW(2,1);
-- Too few operators for the row width (two operators, three columns).
SELECT ROW(1,2,3) OPERATOR(pg_catalog.<, pg_catalog.<) ROW(1,2,4);
-- A list is only meaningful for row (and, later, subquery) comparisons.
-- Scalar infix rejects it.
SELECT 1 OPERATOR(pg_catalog.=, pg_catalog.=) 2;
-- Scalar prefix rejects it too.
SELECT OPERATOR(pg_catalog.-, pg_catalog.-) 1;
-- The array form of ANY/ALL resolves a single operator; a list is rejected.
SELECT 1 OPERATOR(pg_catalog.=, pg_catalog.=) ANY (ARRAY[1,2]);
-- ORDER BY ... USING rejects it.
SELECT 1 FROM public.oq_t ORDER BY a USING OPERATOR(pg_catalog.<, pg_catalog.<);
-- Sublinks reject operator lists in this patch (a later patch enables them).
SELECT (1, 2) OPERATOR(pg_catalog.=, pg_catalog.=) ANY (SELECT 1, 2);
-- DDL definition items (here a CREATE OPERATOR commutator) take a single
-- operator name.  define.c has no ParseState, so this error carries no position.
CREATE OPERATOR ### (leftarg = int4, rightarg = int4, procedure = int4eq,
                     commutator = OPERATOR(pg_catalog.=, alt_ops.=));
\set VERBOSITY default

-- The deparsed row-comparison list must reparse under a restricted search_path
-- (the pg_dump scenario), pinning the same operator OIDs as the original.
BEGIN; SET LOCAL search_path = pg_catalog;
DO $$ BEGIN
  EXECUTE 'CREATE VIEW public.oq_v_rowcmp_reload AS ' || pg_get_viewdef('public.oq_v_rowcmp'::regclass);
  EXECUTE 'CREATE VIEW public.oq_v_rowcmp_mixed_reload AS ' || pg_get_viewdef('public.oq_v_rowcmp_mixed'::regclass);
END $$; COMMIT;
-- The reloaded view resolves alt_ops.< for both columns, same as the original.
SELECT ev_class::regclass FROM pg_rewrite
WHERE ev_action LIKE '%(o ' || 'alt_ops.<(int4,int4)'::regoperator::oid || ' %'
  AND ev_class::regclass::text = 'oq_v_rowcmp_reload'
ORDER BY 1;
-- The reloaded mixed view pins the per-column operators in order: alt_ops.< for
-- column 1 immediately followed by pg_catalog's int4lt for column 2, exactly as
-- the deparsed OPERATOR(alt_ops.<, <) list encodes them.
SELECT ev_class::regclass FROM pg_rewrite
WHERE ev_action LIKE '%(o ' || 'alt_ops.<(int4,int4)'::regoperator::oid
                             || ' ' || 'pg_catalog.<(int4,int4)'::regoperator::oid || '%'
  AND ev_class::regclass::text = 'oq_v_rowcmp_mixed_reload'
ORDER BY 1;
DROP VIEW oq_v_rowcmp_reload;
DROP VIEW oq_v_rowcmp_mixed_reload;
-- NOTE: oq_v_rowcmp and oq_v_rowcmp_mixed are intentionally kept (like the other
-- oq_v_* views) so the pg_upgrade suite dump/restores them and exercises the
-- qualified deparse.
