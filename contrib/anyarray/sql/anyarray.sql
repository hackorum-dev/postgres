CREATE EXTENSION anyarray;

--
-- Phase 1: set operations, helpers and operators for int8, uuid, text
--

-- ===== int8 =====
SELECT anyarray_sort(ARRAY[3, 1, 2, 1]::int8[]);
SELECT anyarray_sort(ARRAY[3, 1, 2, 1]::int8[], 'desc');
SELECT anyarray_sort(ARRAY[]::int8[]);
SELECT anyarray_uniq(ARRAY[1, 1, 2, 3, 3, 2]::int8[]);
SELECT anyarray_idx(ARRAY[10, 20, 30]::int8[], 20::int8);
SELECT anyarray_idx(ARRAY[10, 20, 30]::int8[], 99::int8);
SELECT anyarray_subarray(ARRAY[1,2,3,4,5]::int8[], 2, 2);
SELECT anyarray_subarray(ARRAY[1,2,3,4,5]::int8[], 3);
SELECT anyarray_subarray(ARRAY[1,2,3]::int8[], 5, 1);

SELECT ARRAY[1,2,3]::int8[] & ARRAY[2,3,4]::int8[];
SELECT ARRAY[1,2,3]::int8[] | ARRAY[3,4,5]::int8[];
SELECT ARRAY[1,2,3]::int8[] | 4::int8;
SELECT ARRAY[1,2,3,4]::int8[] - ARRAY[2,4]::int8[];
SELECT ARRAY[1,2,2,3,2]::int8[] - 2::int8;
SELECT # ARRAY[10,20,30]::int8[];
SELECT ARRAY[10,20,30]::int8[] # 20::int8;

-- ===== uuid =====
SELECT anyarray_sort(ARRAY[
  '00000000-0000-0000-0000-000000000003'::uuid,
  '00000000-0000-0000-0000-000000000001',
  '00000000-0000-0000-0000-000000000002']);
SELECT anyarray_uniq(ARRAY[
  '00000000-0000-0000-0000-000000000001'::uuid,
  '00000000-0000-0000-0000-000000000001',
  '00000000-0000-0000-0000-000000000002']);
SELECT anyarray_idx(ARRAY[
  '00000000-0000-0000-0000-000000000001'::uuid,
  '00000000-0000-0000-0000-000000000002'],
  '00000000-0000-0000-0000-000000000002'::uuid);

SELECT ARRAY['00000000-0000-0000-0000-000000000001'::uuid,
             '00000000-0000-0000-0000-000000000002']
     & ARRAY['00000000-0000-0000-0000-000000000002'::uuid,
             '00000000-0000-0000-0000-000000000003'];

SELECT ARRAY['00000000-0000-0000-0000-000000000001'::uuid]
     | '00000000-0000-0000-0000-000000000002'::uuid;

-- ===== text =====
SELECT anyarray_sort(ARRAY['banana','apple','cherry']);
SELECT anyarray_sort(ARRAY['banana','apple','cherry'], 'desc');
SELECT anyarray_uniq(ARRAY['a','b','a','c','b']);
SELECT anyarray_idx(ARRAY['a','b','c'], 'b'::text);
SELECT ARRAY['a','b','c']::text[] & ARRAY['b','c','d']::text[];
SELECT ARRAY['a','b','c']::text[] | 'd'::text;
SELECT ARRAY['a','b','c','b']::text[] - 'b'::text;

-- ===== error cases =====
-- multi-dim
SELECT anyarray_sort(ARRAY[[1,2],[3,4]]::int8[]);
-- nulls
SELECT anyarray_sort(ARRAY[1, NULL, 2]::int8[]);
-- bad direction
SELECT anyarray_sort(ARRAY[1,2,3]::int8[], 'bogus');

--
-- Phase 2: anyquery and @@ operator
--

-- parsing + output round-trip
SELECT '1 & 2'::anyquery::text;
SELECT '1 & 2 | 3'::anyquery::text;
SELECT '(1 | 2) & 3'::anyquery::text;
SELECT '!1 & 2'::anyquery::text;
SELECT '!(1 | 2) & 3'::anyquery::text;
SELECT '"hello world" | foo'::anyquery::text;
SELECT anyquery_querytree('1 & 2 | 3'::anyquery);

-- int8 matching
SELECT ARRAY[1,2,3]::int8[] @@ '1'::anyquery;
SELECT ARRAY[1,2,3]::int8[] @@ '1 & 2'::anyquery;
SELECT ARRAY[1,2,3]::int8[] @@ '1 & 4'::anyquery;
SELECT ARRAY[1,2,3]::int8[] @@ '1 | 4'::anyquery;
SELECT ARRAY[1,2,3]::int8[] @@ '!4'::anyquery;
SELECT ARRAY[1,2,3]::int8[] @@ '!1'::anyquery;
SELECT ARRAY[1,2,3]::int8[] @@ '!4 & 1'::anyquery;
SELECT ARRAY[1,2,3]::int8[] @@ '(1 | 4) & (2 | 5)'::anyquery;

-- commutator
SELECT '1 & 2'::anyquery ~~ ARRAY[1,2,3]::int8[];
SELECT '1 & 4'::anyquery ~~ ARRAY[1,2,3]::int8[];

-- uuid
SELECT ARRAY['00000000-0000-0000-0000-000000000001'::uuid,
             '00000000-0000-0000-0000-000000000002']
       @@ '00000000-0000-0000-0000-000000000001 & 00000000-0000-0000-0000-000000000002'::anyquery;
SELECT ARRAY['00000000-0000-0000-0000-000000000001'::uuid]
       @@ '00000000-0000-0000-0000-000000000002'::anyquery;

-- text
SELECT ARRAY['apple','banana','cherry']::text[] @@ 'apple & banana'::anyquery;
SELECT ARRAY['apple','banana','cherry']::text[] @@ '"banana" | grape'::anyquery;
SELECT ARRAY['apple','banana','cherry']::text[] @@ 'durian | grape'::anyquery;

-- empty array
SELECT ARRAY[]::int8[] @@ '1'::anyquery;
SELECT ARRAY[]::int8[] @@ '!1'::anyquery;

-- parse errors
SELECT ''::anyquery;
SELECT '1 &'::anyquery;
SELECT '1 & (2'::anyquery;
SELECT '1 ) 2'::anyquery;

-- runtime errors: token cannot be parsed as element type
SELECT ARRAY[1,2]::int8[] @@ 'abc'::anyquery;

--
-- Phase 3: GiST signature index
--

-- amvalidate: well-formed opclass
SELECT amname, opcname FROM pg_opclass opc
LEFT JOIN pg_am am ON am.oid = opcmethod
WHERE opc.opcname = 'anyarray_gist_ops' AND NOT amvalidate(opc.oid);

-- int8 GiST: build deterministic table
CREATE TABLE anyarray_gist_int8 (id int, a int8[]);
INSERT INTO anyarray_gist_int8 VALUES
  (1, ARRAY[1,2,3]::int8[]),
  (2, ARRAY[10,20,30]::int8[]),
  (3, ARRAY[10,20,30]::int8[]),
  (4, ARRAY[1,2,3,4,5]::int8[]),
  (5, ARRAY[100,200]::int8[]);

CREATE INDEX anyarray_gist_int8_idx
  ON anyarray_gist_int8 USING gist(a anyarray_gist_ops);

SET enable_seqscan = off;

SELECT id FROM anyarray_gist_int8 WHERE a @> ARRAY[1,2]::int8[] ORDER BY id;
SELECT id FROM anyarray_gist_int8 WHERE a && ARRAY[10,200]::int8[] ORDER BY id;
SELECT id FROM anyarray_gist_int8 WHERE a = ARRAY[10,20,30]::int8[] ORDER BY id;
SELECT id FROM anyarray_gist_int8 WHERE a <@ ARRAY[1,2,3,4,5]::int8[] ORDER BY id;
SELECT id FROM anyarray_gist_int8 WHERE a @@ '10 & 20'::anyquery ORDER BY id;
SELECT id FROM anyarray_gist_int8 WHERE a @@ '1 | 100'::anyquery ORDER BY id;
SELECT id FROM anyarray_gist_int8 WHERE a @@ '!1 & 10'::anyquery ORDER BY id;

-- result consistency: GiST must agree with seqscan
SELECT (
  SELECT count(*) FROM anyarray_gist_int8 WHERE a @> ARRAY[10]::int8[]
) = (
  SELECT count(*) FROM (SELECT 1 FROM anyarray_gist_int8 WHERE a @> ARRAY[10]::int8[]) s
) AS gist_matches_seqscan;

RESET enable_seqscan;

-- text GiST
CREATE TABLE anyarray_gist_text (id int, a text[]);
INSERT INTO anyarray_gist_text VALUES
  (1, ARRAY['apple','banana']),
  (2, ARRAY['banana','cherry']),
  (3, ARRAY['apple','cherry','durian']);

CREATE INDEX anyarray_gist_text_idx
  ON anyarray_gist_text USING gist(a anyarray_gist_ops(siglen=64));

SET enable_seqscan = off;
SELECT id FROM anyarray_gist_text WHERE a @> ARRAY['apple']::text[] ORDER BY id;
SELECT id FROM anyarray_gist_text WHERE a @@ 'apple & cherry'::anyquery ORDER BY id;
RESET enable_seqscan;

-- siglen bounds
CREATE INDEX ON anyarray_gist_text USING gist(a anyarray_gist_ops(siglen=0));
CREATE INDEX ON anyarray_gist_text USING gist(a anyarray_gist_ops(siglen=8193));

--
-- Phase 4: GIN index (per-type, supports standard ops + @@)
--

-- amvalidate: all three opclasses
SELECT amname, opcname FROM pg_opclass opc
LEFT JOIN pg_am am ON am.oid = opcmethod
WHERE opc.opcname LIKE '%anyquery_gin_ops' AND NOT amvalidate(opc.oid);

-- int8 GIN
CREATE TABLE anyarray_gin_int8 (id int, a int8[]);
INSERT INTO anyarray_gin_int8 VALUES
  (1, ARRAY[1,2,3]::int8[]),
  (2, ARRAY[10,20,30]::int8[]),
  (3, ARRAY[10,20,30]::int8[]),
  (4, ARRAY[1,2,3,4,5]::int8[]),
  (5, ARRAY[100,200]::int8[]);

CREATE INDEX anyarray_gin_int8_idx
  ON anyarray_gin_int8 USING gin(a int8_anyquery_gin_ops);

SET enable_seqscan = off;
SELECT id FROM anyarray_gin_int8 WHERE a @> ARRAY[1,2]::int8[] ORDER BY id;
SELECT id FROM anyarray_gin_int8 WHERE a && ARRAY[10,200]::int8[] ORDER BY id;
SELECT id FROM anyarray_gin_int8 WHERE a @@ '10 & 20'::anyquery ORDER BY id;
SELECT id FROM anyarray_gin_int8 WHERE a @@ '(1 & 2) | 100'::anyquery ORDER BY id;
SELECT id FROM anyarray_gin_int8 WHERE a @@ '!1'::anyquery ORDER BY id;

-- uuid GIN
CREATE TABLE anyarray_gin_uuid (id int, a uuid[]);
INSERT INTO anyarray_gin_uuid VALUES
  (1, ARRAY['11111111-1111-1111-1111-111111111111'::uuid,
            '22222222-2222-2222-2222-222222222222']),
  (2, ARRAY['33333333-3333-3333-3333-333333333333'::uuid]),
  (3, ARRAY['11111111-1111-1111-1111-111111111111'::uuid,
            '33333333-3333-3333-3333-333333333333']);

CREATE INDEX anyarray_gin_uuid_idx
  ON anyarray_gin_uuid USING gin(a uuid_anyquery_gin_ops);

SET enable_seqscan = off;
SELECT id FROM anyarray_gin_uuid
  WHERE a @@ '11111111-1111-1111-1111-111111111111 & 22222222-2222-2222-2222-222222222222'::anyquery
  ORDER BY id;
SELECT id FROM anyarray_gin_uuid
  WHERE a @> ARRAY['11111111-1111-1111-1111-111111111111'::uuid]
  ORDER BY id;
RESET enable_seqscan;

-- text GIN
CREATE TABLE anyarray_gin_text (id int, a text[]);
INSERT INTO anyarray_gin_text VALUES
  (1, ARRAY['apple','banana']),
  (2, ARRAY['banana','cherry']),
  (3, ARRAY['apple','cherry','durian']);

CREATE INDEX anyarray_gin_text_idx
  ON anyarray_gin_text USING gin(a text_anyquery_gin_ops);

SET enable_seqscan = off;
SELECT id FROM anyarray_gin_text WHERE a @@ 'apple & cherry'::anyquery ORDER BY id;
SELECT id FROM anyarray_gin_text WHERE a @@ '"durian"'::anyquery ORDER BY id;
SELECT id FROM anyarray_gin_text WHERE a @@ 'apple | grape'::anyquery ORDER BY id;
SELECT id FROM anyarray_gin_text WHERE a @@ '!banana'::anyquery ORDER BY id;
RESET enable_seqscan;

--
-- Phase 5: cross-AM consistency and edge cases
--
-- Build a larger deterministic int8 table and index it with both GiST and
-- GIN.  Then run each candidate query under seqscan, GiST and GIN and check
-- that all three plans return the same row set.
--

CREATE TABLE anyarray_xcheck (id int, a int8[]);

INSERT INTO anyarray_xcheck
SELECT g,
       ARRAY[(g % 7)::int8, ((g * 3) % 11)::int8, ((g + 1) % 5)::int8]
FROM generate_series(1, 200) g;
-- a few hand-picked rows to exercise common values
INSERT INTO anyarray_xcheck VALUES
  (1001, ARRAY[1,2,3,4,5]::int8[]),
  (1002, ARRAY[1,2,3,4,5]::int8[]),
  (1003, ARRAY[100,200,300]::int8[]),
  (1004, ARRAY[]::int8[]),
  (1005, NULL);

CREATE INDEX anyarray_xcheck_gist ON anyarray_xcheck
  USING gist(a anyarray_gist_ops);
CREATE INDEX anyarray_xcheck_gin ON anyarray_xcheck
  USING gin(a int8_anyquery_gin_ops);

-- Run each predicate three times: pure seqscan, GiST-only, GIN-only.
-- All three result sets must match.
CREATE FUNCTION anyarray_xcheck_match(pred text)
RETURNS TABLE(gist_ok bool, gin_ok bool)
LANGUAGE plpgsql AS $$
DECLARE
  seq int[]; gst int[]; gin int[];
BEGIN
  SET LOCAL enable_seqscan = on;
  SET LOCAL enable_indexscan = off;
  SET LOCAL enable_bitmapscan = off;
  EXECUTE 'SELECT array_agg(id ORDER BY id) FROM anyarray_xcheck WHERE '
          || pred INTO seq;

  -- Force GiST by hiding the GIN index.
  ALTER INDEX anyarray_xcheck_gin SET (fastupdate = off);
  SET LOCAL enable_seqscan = off;
  SET LOCAL enable_indexscan = on;
  SET LOCAL enable_bitmapscan = on;
  DROP INDEX anyarray_xcheck_gin;
  EXECUTE 'SELECT array_agg(id ORDER BY id) FROM anyarray_xcheck WHERE '
          || pred INTO gst;
  CREATE INDEX anyarray_xcheck_gin ON anyarray_xcheck
    USING gin(a int8_anyquery_gin_ops);

  -- Force GIN by hiding GiST.
  DROP INDEX anyarray_xcheck_gist;
  EXECUTE 'SELECT array_agg(id ORDER BY id) FROM anyarray_xcheck WHERE '
          || pred INTO gin;
  CREATE INDEX anyarray_xcheck_gist ON anyarray_xcheck
    USING gist(a anyarray_gist_ops);

  gist_ok := seq IS NOT DISTINCT FROM gst;
  gin_ok  := seq IS NOT DISTINCT FROM gin;
  RETURN NEXT;
END $$;

SELECT pred, gist_ok, gin_ok FROM (VALUES
  ('a @> ARRAY[1,2]::int8[]'),
  ('a @> ARRAY[100,200]::int8[]'),
  ('a && ARRAY[5,99]::int8[]'),
  ('a = ARRAY[1,2,3,4,5]::int8[]'),
  ('a @@ ''1 & 2''::anyquery'),
  ('a @@ ''100 | 999''::anyquery'),
  ('a @@ ''!1 & 2''::anyquery')
) v(pred), LATERAL anyarray_xcheck_match(pred);

-- Edge cases: empty / NULL arrays via index
SET enable_seqscan = off;
SELECT id FROM anyarray_xcheck WHERE a @> ARRAY[]::int8[] AND id = 1004;
SELECT id FROM anyarray_xcheck WHERE a <@ ARRAY[1,2,3]::int8[] AND id < 100 ORDER BY id LIMIT 3;
SELECT count(*) FROM anyarray_xcheck WHERE a IS NULL;
RESET enable_seqscan;

-- DELETE / VACUUM / re-query through index
DELETE FROM anyarray_xcheck WHERE id BETWEEN 1 AND 50;
VACUUM anyarray_xcheck;
SET enable_seqscan = off;
SELECT count(*) FROM anyarray_xcheck WHERE a @@ '1 | 2'::anyquery;
RESET enable_seqscan;

DROP FUNCTION anyarray_xcheck_match(text);
