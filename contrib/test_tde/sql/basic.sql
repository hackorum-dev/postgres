-- Basic test for test_tde extension
-- Verify that encryption/decryption works correctly

-- Show current settings
SHOW test_tde.key;

-- Create a test table
CREATE TABLE test_encrypt (
    id serial PRIMARY KEY,
    secret_data text,
    secret_number integer
);

-- Insert some data
INSERT INTO test_encrypt (secret_data, secret_number) VALUES
    ('This is secret data', 12345),
    ('Another secret message', 67890),
    ('PostgreSQL TDE test', 11111);

-- Force a checkpoint to ensure data is written to disk
CHECKPOINT;

-- Read data back - should be decrypted correctly
SELECT * FROM test_encrypt ORDER BY id;

-- Update some data
UPDATE test_encrypt SET secret_data = 'Updated secret' WHERE id = 1;

-- Verify update worked
SELECT * FROM test_encrypt WHERE id = 1;

-- Test with larger data
INSERT INTO test_encrypt (secret_data, secret_number)
SELECT
    repeat('Large data block ', 100),
    generate_series
FROM generate_series(1, 10);

-- Count rows
SELECT COUNT(*) FROM test_encrypt;

-- Test with NULL values
INSERT INTO test_encrypt (secret_data, secret_number) VALUES (NULL, NULL);
SELECT * FROM test_encrypt WHERE secret_data IS NULL;

-- Test index creation (index pages should also be encrypted)
CREATE INDEX ON test_encrypt (secret_number);

-- Use the index
SELECT secret_data FROM test_encrypt WHERE secret_number = 12345;

-- Clean up
DROP TABLE test_encrypt;

-- =============================================================================
-- DDL Tests: Operations that change RelFileNumber
-- These operations create new files and write records through storage hooks,
-- so encryption/decryption works correctly.
-- =============================================================================

-- -----------------------------------------------------------------------------
-- Test 1: TRUNCATE (creates new file, writes through hooks)
-- -----------------------------------------------------------------------------
CREATE TABLE test_truncate (id int, data text);
INSERT INTO test_truncate VALUES (1, 'before truncate');
SELECT * FROM test_truncate;

TRUNCATE test_truncate;

-- Insert new data after truncate - works fine (new file, new encryption through hooks)
INSERT INTO test_truncate VALUES (2, 'after truncate');
SELECT * FROM test_truncate;

DROP TABLE test_truncate;

-- -----------------------------------------------------------------------------
-- Test 2: CLUSTER (rewrites table through hooks)
-- -----------------------------------------------------------------------------
CREATE TABLE test_cluster (id int PRIMARY KEY, data text);
INSERT INTO test_cluster SELECT g, 'data ' || g FROM generate_series(1, 100) g;
CHECKPOINT;

CLUSTER test_cluster USING test_cluster_pkey;

-- Works fine - data rewritten through storage hooks
SELECT COUNT(*) FROM test_cluster;
SELECT * FROM test_cluster WHERE id = 50;

DROP TABLE test_cluster;

-- -----------------------------------------------------------------------------
-- Test 3: VACUUM FULL (rewrites table through hooks)
-- -----------------------------------------------------------------------------
CREATE TABLE test_vacuum_full (id int, data text);
INSERT INTO test_vacuum_full SELECT g, 'data ' || g FROM generate_series(1, 100) g;
DELETE FROM test_vacuum_full WHERE id > 50;
CHECKPOINT;

VACUUM FULL test_vacuum_full;

-- Works fine - data rewritten through storage hooks
SELECT COUNT(*) FROM test_vacuum_full;

DROP TABLE test_vacuum_full;

-- -----------------------------------------------------------------------------
-- Test 4: REINDEX (rebuilds index through hooks)
-- -----------------------------------------------------------------------------
CREATE TABLE test_reindex (id int PRIMARY KEY, data text);
INSERT INTO test_reindex SELECT g, 'data ' || g FROM generate_series(1, 100) g;
CHECKPOINT;

REINDEX INDEX test_reindex_pkey;

-- Works fine - index rebuilt through storage hooks
SET enable_seqscan = off;
SELECT * FROM test_reindex WHERE id = 50;
RESET enable_seqscan;

DROP TABLE test_reindex;

-- =============================================================================
-- Additional DDL Tests: Operations that change RelFileNumber or copy files
-- These also go through storage hooks, so encryption/decryption works correctly.
-- =============================================================================

-- -----------------------------------------------------------------------------
-- Test 5: ALTER TABLE SET TABLESPACE
-- RelFileNumber changes, but data is copied through storage hooks
-- -----------------------------------------------------------------------------
SET allow_in_place_tablespaces = true;
CREATE TABLESPACE regress_tde_tblspc LOCATION '';

CREATE TABLE test_set_tablespace (id int, data text);
INSERT INTO test_set_tablespace SELECT g, 'data ' || g FROM generate_series(1, 50) g;
CHECKPOINT;

-- Move to different tablespace - data copied through storage hooks
ALTER TABLE test_set_tablespace SET TABLESPACE regress_tde_tblspc;

-- Works fine - data was re-encrypted with new RelFileNumber
SELECT COUNT(*) FROM test_set_tablespace;

DROP TABLE test_set_tablespace;
DROP TABLESPACE regress_tde_tblspc;
