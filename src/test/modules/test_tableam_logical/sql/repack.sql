--
-- REPACK (CONCURRENTLY) needs the table AM's relation_supports_logical_decoding
-- callback.  The test AMs copy heap's routine, which heap's index builds and
-- TOAST code reject, so their tables have no index and no TOAST table.
--
CREATE EXTENSION test_tableam_logical;

CREATE TABLE repack_heap (i int PRIMARY KEY, j int);
CREATE TABLE repack_decode (i int, j int) USING test_decode;
CREATE TABLE repack_nodecode (i int, j int) USING test_nodecode;
INSERT INTO repack_heap SELECT g, g * 2 FROM generate_series(1, 100) g;
INSERT INTO repack_decode SELECT g, g * 2 FROM generate_series(1, 100) g;
INSERT INTO repack_nodecode SELECT g, g * 2 FROM generate_series(1, 100) g;

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

DROP TABLE repack_heap, repack_decode, repack_nodecode;
DROP EXTENSION test_tableam_logical;
