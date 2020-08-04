CREATE TABLE ltable (a int, b real);
CREATE FOREIGN TABLE ftable (a int) server loopback options (table_name 'ltable');
VACUUM ANALYZE;

-- Check statistic interface routine on an empty table.
SELECT * FROM extract_relation_statistics('ltable');
SELECT * FROM extract_relation_statistics('ftable');

-- Check statistic interface routine on non-empty tables.
INSERT INTO ltable (a, b) (SELECT *, 1.01 FROM generate_series(1, 1E4));
-- hereinafter we don't want to depend on analyze order. If table will be
-- analyzed before ltable than we got out-of-date statistic.
ANALYZE ltable;
ANALYZE;
SELECT * FROM extract_relation_statistics('ltable');

-- Check ANALYZE on foreign table
INSERT INTO ltable (a, b) (SELECT *, 2.01 FROM generate_series(1E4, 2E4));
ANALYZE ltable;
EXPLAIN (TIMING OFF, SUMMARY OFF, COSTS ON, ANALYZE) SELECT * FROM ltable;
EXPLAIN (TIMING OFF, SUMMARY OFF, COSTS ON, ANALYZE) SELECT * FROM ftable;
ANALYZE;
EXPLAIN (TIMING OFF, SUMMARY OFF, COSTS ON, ANALYZE) SELECT * FROM ftable;

-- Check selectivity
EXPLAIN (TIMING OFF, SUMMARY OFF, COSTS ON, ANALYZE) SELECT * FROM ltable WHERE a > 10;
EXPLAIN (TIMING OFF, SUMMARY OFF, COSTS ON, ANALYZE) SELECT * FROM ftable WHERE a > 10;

-- Check new attribute
ALTER TABLE ltable ADD COLUMN c int DEFAULT 42;
ALTER TABLE ftable ADD COLUMN c int;
EXPLAIN (TIMING OFF, SUMMARY OFF, COSTS ON, ANALYZE) SELECT * FROM ltable WHERE a > 10 AND c < 15;
EXPLAIN (TIMING OFF, SUMMARY OFF, COSTS ON, ANALYZE) SELECT * FROM ftable WHERE a > 10 AND c < 15;

ANALYZE ltable;
ANALYZE;
EXPLAIN (TIMING OFF, SUMMARY OFF, COSTS ON, ANALYZE) SELECT * FROM ltable WHERE a > 10 AND c < 15;
EXPLAIN (TIMING OFF, SUMMARY OFF, COSTS ON, ANALYZE) SELECT * FROM ftable WHERE a > 10 AND c < 15;

-- Test default vacuum analyzes foreign relation
INSERT INTO ltable (a, b) (SELECT *, 2.01 FROM generate_series(2E4, 3E4));
ANALYZE ltable;
VACUUM ANALYZE;
EXPLAIN (TIMING OFF, SUMMARY OFF, COSTS ON, ANALYZE) SELECT * FROM ltable WHERE a > 100 AND c < 43;
EXPLAIN (TIMING OFF, SUMMARY OFF, COSTS ON, ANALYZE) SELECT * FROM ftable WHERE a > 100 AND c < 43;

