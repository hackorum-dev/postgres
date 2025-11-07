-- Test condition variable-based WAL waiting for logical decoding

-- Setup: Create a logical replication slot
SELECT 'init' FROM pg_create_logical_replication_slot('regress_cv_test_slot', 'test_decoding');

-- Create test table
CREATE TABLE regress_cv_test (id SERIAL PRIMARY KEY, data TEXT);

-- Test 1: Basic functionality - insert and decode (should not wait)
BEGIN;
INSERT INTO regress_cv_test (data) VALUES ('test1');
COMMIT;

-- Verify we can read the changes
SELECT data FROM pg_logical_slot_get_changes('regress_cv_test_slot', NULL, NULL) 
WHERE data LIKE '%test1%';

-- Test 2: Multiple transactions
BEGIN;
INSERT INTO regress_cv_test (data) VALUES ('test2');
INSERT INTO regress_cv_test (data) VALUES ('test3');
COMMIT;

SELECT count(*) FROM pg_logical_slot_get_changes('regress_cv_test_slot', NULL, NULL) 
WHERE data LIKE '%test%';

-- Test 3: Verify slot advancement
SELECT slot_name, confirmed_flush_lsn IS NOT NULL AS has_flush_lsn
FROM pg_replication_slots 
WHERE slot_name = 'regress_cv_test_slot';

-- Cleanup
SELECT pg_drop_replication_slot('regress_cv_test_slot');
DROP TABLE regress_cv_test;

