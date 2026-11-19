--
-- REPACK (CONCURRENTLY) needs the table AM to say it supports logical
-- decoding, through its relation_supports_features callback.  The test AMs
-- copy heap's routine, which heap's index builds and TOAST code reject, so
-- their tables have no index and no TOAST table.
--
CREATE EXTENSION test_tableam_logical;

CREATE TABLE repack_heap (i int PRIMARY KEY, j int);
CREATE TABLE repack_decode (i int, j int) USING test_decode;
CREATE TABLE repack_nodecode (i int, j int) USING test_nodecode;
INSERT INTO repack_heap SELECT g, g * 2 FROM generate_series(1, 100) g;
INSERT INTO repack_decode SELECT g, g * 2 FROM generate_series(1, 100) g;
INSERT INTO repack_nodecode SELECT g, g * 2 FROM generate_series(1, 100) g;

-- What the callback returns is cut down to the bits asked for: bit 0 is
-- TABLEAM_SUPPORTS_LOGICAL_DECODING, and 2 is a flag no AM has heard of.
-- No callback, none; asking for nothing, nothing.
CREATE TABLE repack_overclaim (i int, j int) USING test_overclaim;
SELECT c.relname, test_tableam_supports_features(c.oid, 1) AS asked_1,
       test_tableam_supports_features(c.oid, 3) AS asked_3,
       test_tableam_supports_features(c.oid, 2) AS asked_2,
       test_tableam_supports_features(c.oid, 0) AS asked_0
  FROM pg_class c WHERE c.relkind = 'r' AND c.relname LIKE 'repack\_%' ORDER BY c.relname;

-- Not every relation has a table AM to ask.
CREATE TABLE repack_part (i int) PARTITION BY RANGE (i);
CREATE VIEW repack_view AS SELECT 1 AS i;
SELECT test_tableam_supports_features('repack_part'::regclass, 1);
SELECT test_tableam_supports_features('repack_view'::regclass, 1);
SELECT test_tableam_supports_features('repack_heap_pkey'::regclass, 1);

-- heap has it
REPACK (CONCURRENTLY) repack_heap;

-- an AM without it is refused
REPACK (CONCURRENTLY) repack_nodecode;

-- one with it passes that check, then fails for want of an identity index
REPACK (CONCURRENTLY) repack_decode;

-- the rest of REPACK doesn't need it
REPACK repack_nodecode;
VACUUM FULL repack_nodecode;

SELECT count(*) FROM repack_heap;
SELECT count(*) FROM repack_decode;
SELECT count(*) FROM repack_nodecode;

DROP VIEW repack_view;
DROP TABLE repack_part;
DROP TABLE repack_heap, repack_decode, repack_nodecode, repack_overclaim;
DROP EXTENSION test_tableam_logical;
