-- Create logical replication slot
SELECT 'init' FROM pg_create_logical_replication_slot('regress_cv_test_slot', 'test_decoding');

-- Create test table
CREATE TABLE regress_cv_test (id SERIAL, data TEXT);

-- Test: Empty reads (no waiting should occur)
SELECT count(*) FROM pg_logical_slot_get_changes('regress_cv_test_slot', NULL, NULL);

-- Test: Reading from current position (no waiting)
SELECT pg_current_wal_lsn() AS current_lsn \gset
SELECT count(*) FROM pg_logical_slot_get_changes('regress_cv_test_slot', :'current_lsn', NULL);

-- Insert some test data to see actual changes
INSERT INTO regress_cv_test (data) VALUES ('test1'), ('test2');

-- Test: Read the actual changes
SELECT data FROM pg_logical_slot_get_changes('regress_cv_test_slot', NULL, NULL) 
WHERE data LIKE '%test%';

-- Cleanup
SELECT pg_drop_replication_slot('regress_cv_test_slot');
DROP TABLE regress_cv_test;
