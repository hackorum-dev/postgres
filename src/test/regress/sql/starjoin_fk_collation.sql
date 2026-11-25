--
-- Starjoin lookup semantics under different collations
--
SELECT getdatabaseencoding() <> 'UTF8' OR NOT EXISTS
  (SELECT FROM pg_collation WHERE collprovider = 'i' AND collname <> 'unicode')
  AS skip_test \gset
\if :skip_test
\quit
\endif

BEGIN;
SET LOCAL enable_starjoin_ordering = on;

CREATE TEMP TABLE d1 (id int PRIMARY KEY);
CREATE TEMP TABLE d2 (id int PRIMARY KEY);
CREATE TEMP TABLE d3 (id int PRIMARY KEY);
CREATE TEMP TABLE d4 (id int PRIMARY KEY);
INSERT INTO d1 VALUES (1);
INSERT INTO d2 VALUES (1);
INSERT INTO d3 VALUES (1);
INSERT INTO d4 VALUES (1);
CREATE COLLATION pg_temp.starjoin_ci
  (provider = icu, locale = 'und-u-ks-level2', deterministic = false);
CREATE TEMP TABLE spellings (k text COLLATE "C" PRIMARY KEY);
INSERT INTO spellings VALUES ('a'), ('A');
CREATE TEMP TABLE fact (
  a int NOT NULL REFERENCES d1, b int NOT NULL REFERENCES d2,
  c int NOT NULL REFERENCES d3, d int NOT NULL REFERENCES d4,
  k text COLLATE "C" NOT NULL CONSTRAINT exact_fk REFERENCES spellings
);
INSERT INTO fact SELECT 1, 1, 1, 1, 'a' FROM generate_series(1, 100);

SELECT $$SELECT count(*) FROM fact f JOIN spellings s ON f.k = s.k
  JOIN d1 ON f.a = d1.id JOIN d2 ON f.b = d2.id
  JOIN d3 ON f.c = d3.id JOIN d4 ON f.d = d4.id$$ AS q_native \gset
SELECT replace(:'q_native', 'f.k = s.k', 'f.k COLLATE pg_temp.starjoin_ci = s.k')
  AS q_folded \gset
SELECT (:q_native) AS exact_matches, (:q_folded) AS case_insensitive_matches;

-- If all dimensions qualify, the join search builds only the join rels of
-- the canonical order, which is the order the query lists them in.  It then
-- needs about as much planner memory as with join_collapse_limit = 1, which
-- forces that order.  A dimension that does not qualify can also be joined
-- at any other point, so the search takes noticeably more memory.  The four
-- dimensions d1 to d4 always qualify and widen the difference.
CREATE FUNCTION pg_temp.dimensions_qualify(query text) RETURNS bool
LANGUAGE plpgsql SET join_collapse_limit = 8 AS $$
DECLARE
  plan json;
  search_memory bigint;
BEGIN
  -- the first planning after DDL also fills the caches
  EXECUTE 'EXPLAIN ' || query;
  EXECUTE 'EXPLAIN (MEMORY, FORMAT JSON) ' || query INTO plan;
  search_memory := plan->0->'Planning'->>'Memory Used';
  SET LOCAL join_collapse_limit = 1;
  EXECUTE 'EXPLAIN (MEMORY, FORMAT JSON) ' || query INTO plan;
  RETURN search_memory <= (plan->0->'Planning'->>'Memory Used')::bigint * 1.1;
END;
$$;

-- Only comparisons with the same equality semantics are single-row lookups.
SELECT pg_temp.dimensions_qualify(:'q_native') AS exact_qualifies,
       NOT pg_temp.dimensions_qualify(:'q_folded') AS case_insensitive_does_not_qualify,
       pg_temp.dimensions_qualify(replace(:'q_native', 'f.k = s.k',
                                          'f.k COLLATE "POSIX" = s.k')) AS deterministic_qualifies;
SET LOCAL enable_starjoin_ordering = off;
SELECT NOT pg_temp.dimensions_qualify(:'q_native') AS feature_off_does_not_qualify;
SET LOCAL enable_starjoin_ordering = on;
ALTER TABLE fact DROP CONSTRAINT exact_fk;
SELECT NOT pg_temp.dimensions_qualify(:'q_native') AS without_text_fk_does_not_qualify;

-- A non-mergejoinable equality stays in joininfo instead of forming an
-- equivalence class.  Check every such clause matched to the FK column.
CREATE OPERATOR pg_temp.=== (LEFTARG = text, RIGHTARG = text, FUNCTION = texteq);
CREATE OPERATOR CLASS pg_temp.loose_ops FOR TYPE text USING btree AS
  OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 pg_temp.===, OPERATOR 4 >=, OPERATOR 5 >,
  FUNCTION 1 bttextcmp(text, text);
ALTER TABLE spellings DROP CONSTRAINT spellings_pkey;
CREATE UNIQUE INDEX ON spellings (k pg_temp.loose_ops);
ALTER TABLE fact ADD FOREIGN KEY (k) REFERENCES spellings (k);
SELECT replace(:'q_native', 'f.k = s.k', 's.k OPERATOR(pg_temp.===) f.k') AS q_loose \gset
SELECT pg_temp.dimensions_qualify(:'q_loose') AS loose_native_qualifies,
       NOT pg_temp.dimensions_qualify(:'q_loose' || ' WHERE s.k COLLATE pg_temp.starjoin_ci
                                          OPERATOR(pg_temp.===) f.k') AS loose_mismatch_does_not_qualify;

-- A nondeterministic collation is fine if it agrees with both proofs.
-- The index columns are deliberately in a different order from the FK.
CREATE TEMP TABLE labels (k text COLLATE pg_temp.starjoin_ci, tag text COLLATE "C");
CREATE UNIQUE INDEX labels_key ON labels (tag, k);
CREATE TEMP TABLE refs (
  a int NOT NULL REFERENCES d1, b int NOT NULL REFERENCES d2,
  c int NOT NULL REFERENCES d3, d int NOT NULL REFERENCES d4,
  k text COLLATE pg_temp.starjoin_ci NOT NULL, tag text COLLATE "C" NOT NULL,
  CONSTRAINT label_fk FOREIGN KEY (k, tag) REFERENCES labels (k, tag)
);
INSERT INTO labels VALUES ('A', 'x');
INSERT INTO refs VALUES (1, 1, 1, 1, 'a', 'x');
SELECT $$SELECT count(*) FROM refs f JOIN labels l ON f.k = l.k AND f.tag = l.tag
  JOIN d1 ON f.a = d1.id JOIN d2 ON f.b = d2.id
  JOIN d3 ON f.c = d3.id JOIN d4 ON f.d = d4.id$$ AS q_label \gset
SELECT pg_temp.dimensions_qualify(:'q_label') AS matching_nondeterministic_qualifies;

-- Now only the index is case-sensitive.  A join using its collation
-- can miss the referenced row; one using the column's can match two.
ALTER TABLE refs DROP CONSTRAINT label_fk;
DROP INDEX labels_key;
CREATE UNIQUE INDEX labels_key ON labels (tag, k COLLATE "C");
ALTER TABLE refs ADD FOREIGN KEY (k, tag) REFERENCES labels (k, tag);
SELECT replace(:'q_label', 'f.k = l.k', 'f.k COLLATE "C" = l.k') AS q_index \gset
SELECT (:q_index) AS index_collation_matches,
       pg_temp.dimensions_qualify(:'q_index') AS qualifies;
INSERT INTO labels VALUES ('a', 'x');
SELECT (:q_label) AS column_collation_matches,
       pg_temp.dimensions_qualify(:'q_label') AS qualifies;
ROLLBACK;
