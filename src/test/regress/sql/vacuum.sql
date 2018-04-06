--
-- VACUUM
--

CREATE FUNCTION wait_barrier() RETURNS void LANGUAGE plpgsql AS $$
DECLARE vxids text[];
BEGIN
 -- wait for all transactions to commit to make deleted tuples vacuumable
 SELECT array_agg(DISTINCT virtualtransaction) FROM pg_locks WHERE pid <> pg_backend_pid() INTO vxids;
 WHILE (SELECT count(virtualtransaction) FROM pg_locks WHERE pid <> pg_backend_pid() AND virtualtransaction = ANY(vxids)) > 0 LOOP
    PERFORM pg_sleep(0.1);
 END LOOP;
END
$$;

CREATE TABLE vactst (i INT) WITH (autovacuum_enabled = false);
INSERT INTO vactst VALUES (1);
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst VALUES (0);
SELECT count(*) FROM vactst;
DELETE FROM vactst WHERE i != 0;
SELECT wait_barrier();
SELECT * FROM vactst;
VACUUM FULL vactst;
UPDATE vactst SET i = i + 1;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst SELECT * FROM vactst;
INSERT INTO vactst VALUES (0);
SELECT count(*) FROM vactst;
DELETE FROM vactst WHERE i != 0;
SELECT wait_barrier();
VACUUM (FULL) vactst;
DELETE FROM vactst;
SELECT wait_barrier();
SELECT * FROM vactst;

VACUUM (FULL, FREEZE) vactst;
VACUUM (ANALYZE, FULL) vactst;

CREATE TABLE vaccluster (i INT PRIMARY KEY);
ALTER TABLE vaccluster CLUSTER ON vaccluster_pkey;
CLUSTER vaccluster;

CREATE FUNCTION do_analyze() RETURNS VOID VOLATILE LANGUAGE SQL
	AS 'ANALYZE pg_am';
CREATE FUNCTION wrap_do_analyze(c INT) RETURNS INT IMMUTABLE LANGUAGE SQL
	AS 'SELECT $1 FROM do_analyze()';
CREATE INDEX ON vaccluster(wrap_do_analyze(i));
INSERT INTO vaccluster VALUES (1), (2);
ANALYZE vaccluster;

VACUUM FULL pg_am;
VACUUM FULL pg_class;
VACUUM FULL pg_database;
VACUUM FULL vaccluster;
VACUUM FULL vactst;

VACUUM (DISABLE_PAGE_SKIPPING) vaccluster;

-- partitioned table
CREATE TABLE vacparted (a int, b char) PARTITION BY LIST (a);
CREATE TABLE vacparted1 PARTITION OF vacparted FOR VALUES IN (1);
INSERT INTO vacparted VALUES (1, 'a');
UPDATE vacparted SET b = 'b';
VACUUM (ANALYZE) vacparted;
VACUUM (FULL) vacparted;
VACUUM (FREEZE) vacparted;

-- check behavior with duplicate column mentions
VACUUM ANALYZE vacparted(a,b,a);
ANALYZE vacparted(a,b,b);

-- multiple tables specified
VACUUM vaccluster, vactst;
VACUUM vacparted, does_not_exist;
VACUUM (FREEZE) vacparted, vaccluster, vactst;
VACUUM (FREEZE) does_not_exist, vaccluster;
VACUUM ANALYZE vactst, vacparted (a);
VACUUM ANALYZE vactst (does_not_exist), vacparted (b);
VACUUM FULL vacparted, vactst;
VACUUM FULL vactst, vacparted (a, b), vaccluster (i);
ANALYZE vactst, vacparted;
ANALYZE vacparted (b), vactst;
ANALYZE vactst, does_not_exist, vacparted;
ANALYZE vactst (i), vacparted (does_not_exist);

-- parenthesized syntax for ANALYZE
ANALYZE (VERBOSE) does_not_exist;
ANALYZE (nonexistant-arg) does_not_exist;

-- large mwm vacuum runs
CREATE UNLOGGED TABLE vactst2 (i INT) WITH (autovacuum_enabled = false);
INSERT INTO vactst2 SELECT * from generate_series(1,300000);
CREATE INDEX ix_vactst2 ON vactst2 (i);
DELETE FROM vactst2 WHERE i % 4 != 1;
SET maintenance_work_mem = 1024;
SELECT wait_barrier();
VACUUM vactst2;
SET maintenance_work_mem TO DEFAULT;
DROP INDEX ix_vactst2;
TRUNCATE TABLE vactst2;

INSERT INTO vactst2 SELECT * from generate_series(1,40);
CREATE INDEX ix_vactst2 ON vactst2 (i);
DELETE FROM vactst2;
SELECT wait_barrier();
VACUUM vactst2;
EXPLAIN (ANALYZE, BUFFERS, COSTS off, TIMING off, SUMMARY off) SELECT * FROM vactst2;
SELECT count(*) FROM vactst2;
DROP INDEX ix_vactst2;
TRUNCATE TABLE vactst2;

DROP TABLE vaccluster;
DROP TABLE vactst2;
DROP TABLE vactst;
DROP TABLE vacparted;
DROP FUNCTION wait_barrier();
