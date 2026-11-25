-- Starjoin lookup semantics with a collation-sensitive FK operator class
\set ON_ERROR_STOP on
SELECT getdatabaseencoding() <> 'UTF8' OR icu_unicode_version() IS NULL AS skip_test \gset
\if :skip_test
\echo 'SKIP: requires ICU and a UTF8 database'
\quit
\endif

BEGIN;
SET LOCAL geqo = off;
SET LOCAL enable_starjoin_ordering = on;

CREATE TEMP TABLE d1 (id int PRIMARY KEY);
CREATE TEMP TABLE d2 (id int PRIMARY KEY);
CREATE TEMP TABLE d3 (id int PRIMARY KEY);
CREATE TEMP TABLE d4 (id int PRIMARY KEY);
INSERT INTO d1 VALUES (1);
INSERT INTO d2 VALUES (1);
INSERT INTO d3 VALUES (1);
INSERT INTO d4 VALUES (1);
CREATE COLLATION pg_temp.en_det (provider = icu, locale = 'en', deterministic = true);
CREATE COLLATION pg_temp.tr_det (provider = icu, locale = 'tr', deterministic = true);

CREATE FUNCTION pg_temp.fold_cmp(a text, b text) RETURNS int
LANGUAGE plpgsql IMMUTABLE STRICT AS $$
BEGIN
  RETURN bttextcmp(lower(a), lower(b));
END;
$$;
-- Derive all five strategies from the same comparator. Keep the equality
-- function non-inlinable so the join clause contains the actual FK operator.
DO $$
DECLARE
  s record;
BEGIN
  FOR s IN SELECT * FROM (VALUES
    ('fold_lt', '~<~', '<'), ('fold_le', '~<=~', '<='), ('fold_eq', '===', '='),
    ('fold_ge', '~>=~', '>='), ('fold_gt', '~>~', '>')) v(fn, op, cmp)
  LOOP
    EXECUTE format('CREATE FUNCTION pg_temp.%I(text, text) RETURNS bool
                    LANGUAGE plpgsql IMMUTABLE STRICT AS %L', s.fn,
                   format('BEGIN RETURN pg_temp.fold_cmp($1, $2) %s 0; END;', s.cmp));
    EXECUTE format('CREATE OPERATOR pg_temp.%s
                    (LEFTARG = text, RIGHTARG = text, FUNCTION = pg_temp.%I)', s.op, s.fn);
  END LOOP;
END;
$$;
CREATE OPERATOR CLASS pg_temp.fold_ops FOR TYPE text USING btree AS
  OPERATOR 1 pg_temp.~<~, OPERATOR 2 pg_temp.~<=~, OPERATOR 3 pg_temp.===,
  OPERATOR 4 pg_temp.~>=~, OPERATOR 5 pg_temp.~>~, FUNCTION 1 pg_temp.fold_cmp(text, text);

CREATE TEMP TABLE words (k text COLLATE pg_temp.en_det NOT NULL);
CREATE UNIQUE INDEX words_key ON words (k pg_temp.fold_ops);
INSERT INTO words VALUES ('I'), (U&'\0131'); -- dotless i
CREATE TEMP TABLE fact (
  k text COLLATE pg_temp.en_det NOT NULL CONSTRAINT words_fk REFERENCES words(k),
  a int NOT NULL REFERENCES d1, b int NOT NULL REFERENCES d2,
  c int NOT NULL REFERENCES d3, d int NOT NULL REFERENCES d4
);
INSERT INTO fact SELECT 'I', 1, 1, 1, 1 FROM generate_series(1, 100);
ANALYZE fact, words;

SELECT $$SELECT count(*) FROM fact f JOIN words w ON w.k OPERATOR(pg_temp.===) f.k
  JOIN d1 ON f.a = d1.id JOIN d2 ON f.b = d2.id
  JOIN d3 ON f.c = d3.id JOIN d4 ON f.d = d4.id$$ AS q_en \gset
SELECT replace(:'q_en', 'w.k OPERATOR', 'w.k COLLATE pg_temp.tr_det OPERATOR') AS q_tr \gset
-- 100 facts; 100 matches in English, 200 in Turkish.
SELECT (SELECT count(*) FROM fact) AS facts, (:q_en) AS en_matches, (:q_tr) AS tr_matches;

-- As in starjoin_fk_collation, the query lists the dimensions in canonical
-- order. If all qualify, the search needs about as much planner memory as
-- forcing that order; otherwise it builds additional joinrels.
CREATE FUNCTION pg_temp.dimensions_qualify(query text) RETURNS bool
LANGUAGE plpgsql SET join_collapse_limit = 8 AS $$
DECLARE
  plan json;
  search_memory bigint;
BEGIN
  EXECUTE 'EXPLAIN ' || query;
  EXECUTE 'EXPLAIN (MEMORY, FORMAT JSON) ' || query INTO plan;
  search_memory := plan->0->'Planning'->>'Memory Used';
  SET LOCAL join_collapse_limit = 1;
  EXECUTE 'EXPLAIN (MEMORY, FORMAT JSON) ' || query INTO plan;
  RETURN search_memory <= (plan->0->'Planning'->>'Memory Used')::bigint * 1.1;
END;
$$;
-- The Turkish equality is not a single-row lookup.
SELECT pg_temp.dimensions_qualify(:'q_en') AS en_qualifies,
       pg_temp.dimensions_qualify(:'q_tr') AS tr_qualifies;
SET LOCAL enable_starjoin_ordering = off;
SELECT pg_temp.dimensions_qualify(:'q_en') AS qualifies_with_feature_off;
SET LOCAL enable_starjoin_ordering = on;

SAVEPOINT change_index;
ALTER TABLE fact DROP CONSTRAINT words_fk;
SELECT pg_temp.dimensions_qualify(:'q_en') AS qualifies_without_words_fk;

-- A mismatch with just the backing index can also invalidate uniqueness.
DROP INDEX words_key;
TRUNCATE words;
CREATE UNIQUE INDEX words_key ON words (k COLLATE pg_temp.tr_det pg_temp.fold_ops);
INSERT INTO words VALUES ('I'), ('i');
ALTER TABLE fact ADD FOREIGN KEY (k) REFERENCES words(k);
SELECT (:q_en) AS en_matches_with_tr_index,
       pg_temp.dimensions_qualify(:'q_en') AS qualifies;
ROLLBACK TO change_index;

-- A deterministic source collation need not imply equality under a
-- nondeterministic target, either. English folds 'I' to 'i'; Turkish
-- folds it to dotless i. Test existence and uniqueness separately.
ALTER TABLE fact DROP CONSTRAINT words_fk;
DROP INDEX words_key;
TRUNCATE words;
CREATE COLLATION pg_temp.tr_nd (provider = icu, locale = 'tr', deterministic = false);
CREATE UNIQUE INDEX words_key ON words (k COLLATE pg_temp.tr_nd pg_temp.fold_ops);
INSERT INTO words VALUES ('I');
UPDATE fact SET k = 'i';
ALTER TABLE fact ADD FOREIGN KEY (k) REFERENCES words(k);
SELECT replace(:'q_en', 'w.k OPERATOR', 'w.k COLLATE pg_temp.tr_nd OPERATOR') AS q_nd \gset
SELECT (:q_nd) AS matches_under_index_collation,
       pg_temp.dimensions_qualify(:'q_nd') AS qualifies;
INSERT INTO words VALUES ('i');
SELECT (:q_en) AS matches_under_column_collation,
       pg_temp.dimensions_qualify(:'q_en') AS qualifies;
ROLLBACK TO change_index;

-- Match the same key through an equivalence class instead of a loose qual.
ALTER OPERATOR pg_temp.=== (text, text) SET (COMMUTATOR = OPERATOR(pg_temp.===), MERGES);
SELECT pg_temp.dimensions_qualify(:'q_en') AS ec_en_qualifies,
       pg_temp.dimensions_qualify(:'q_tr') AS ec_tr_qualifies;
ROLLBACK;
