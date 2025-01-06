SELECT pg_sleep(2);
CREATE TABLE t0(i int, t text);
ALTER TABLE t0 ALTER t SET STORAGE PLAIN;
INSERT INTO t0 SELECT g, g || repeat('x', 8000) FROM generate_series(1, 256) g;

CREATE TABLE t1(i int, t text);
ALTER TABLE tl ALTER t SET STORAGE PLAIN;
INSERT INTO t1 SELECT g, g || repeat('x', 8000) FROM generate_series(1, 256) g;

EXPLAIN ANALYZE SELECT SUM(i) FROM t0 GROUP BY t;
EXPLAIN ANALYZE SELECT SUM(i) FROM t1 GROUP BY t;

\c -
SELECT 'SELECT pg_sleep(1);' FROM generate_series(1, 10)
\gexec

BEGIN;
SELECT 'EXPLAIN ANALYZE SELECT SUM(i) FROM t0; EXPLAIN ANALYZE SELECT SUM(i) FROM t1;' FROM generate_series(1, 20)
\gexec

SELECT pg_sleep(10);
ROLLBACK;
