--
-- TRUNCATE foreign key lookups through the heap scan of pg_constraint
-- (ignore_system_indexes), where systable_beginscan() has to check the
-- confrelid = ANY key itself.
--
CREATE TABLE trunc_isi_a (id int PRIMARY KEY) PARTITION BY RANGE (id);
CREATE TABLE trunc_isi_a1 PARTITION OF trunc_isi_a FOR VALUES FROM (0) TO (10);
CREATE TABLE trunc_isi_b (id int PRIMARY KEY, a int REFERENCES trunc_isi_a)
  PARTITION BY RANGE (id);
CREATE TABLE trunc_isi_b1 PARTITION OF trunc_isi_b FOR VALUES FROM (0) TO (10);
CREATE TABLE trunc_isi_c (b int REFERENCES trunc_isi_b);
INSERT INTO trunc_isi_a VALUES (1);
INSERT INTO trunc_isi_b VALUES (1, 1);
INSERT INTO trunc_isi_c VALUES (1);
-- ignore_system_indexes only takes effect at connection start.  Only SHOW,
-- the TRUNCATEs and a count run with it: DDL may use an index anyway and
-- warn.
\c -reuse-previous=on "options=-cignore_system_indexes=on"
SHOW ignore_system_indexes;
TRUNCATE trunc_isi_a1, trunc_isi_b1;  -- fail
TRUNCATE trunc_isi_a1 CASCADE;
SELECT (SELECT count(*) FROM trunc_isi_a) + (SELECT count(*) FROM trunc_isi_b) +
       (SELECT count(*) FROM trunc_isi_c) AS rows_left;
\c -reuse-previous=on "options="
SHOW ignore_system_indexes;
DROP TABLE trunc_isi_a, trunc_isi_b, trunc_isi_c;
