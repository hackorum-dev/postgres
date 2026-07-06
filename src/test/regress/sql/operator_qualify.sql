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
