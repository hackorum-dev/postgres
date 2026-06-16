-- float8 check

CREATE TABLE float8tmp (a float8);

\copy float8tmp from 'data/float8.data'

SET enable_seqscan=on;

SELECT count(*) FROM float8tmp WHERE a <  -1890.0;

SELECT count(*) FROM float8tmp WHERE a <= -1890.0;

SELECT count(*) FROM float8tmp WHERE a  = -1890.0;

SELECT count(*) FROM float8tmp WHERE a >= -1890.0;

SELECT count(*) FROM float8tmp WHERE a >  -1890.0;

SELECT a, a <-> '-1890.0' FROM float8tmp ORDER BY a <-> '-1890.0' LIMIT 3;

CREATE INDEX float8idx ON float8tmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM float8tmp WHERE a <  -1890.0::float8;

SELECT count(*) FROM float8tmp WHERE a <= -1890.0::float8;

SELECT count(*) FROM float8tmp WHERE a  = -1890.0::float8;

SELECT count(*) FROM float8tmp WHERE a >= -1890.0::float8;

SELECT count(*) FROM float8tmp WHERE a >  -1890.0::float8;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '-1890.0' FROM float8tmp ORDER BY a <-> '-1890.0' LIMIT 3;
SELECT a, a <-> '-1890.0' FROM float8tmp ORDER BY a <-> '-1890.0' LIMIT 3;

-- NaN and infinities must give identical results via seqscan and index scan;
-- NaN sorts after every non-NaN value and is equal to itself.
INSERT INTO float8tmp VALUES ('NaN'), ('Infinity'), ('-Infinity');

SET enable_seqscan=on;
SET enable_indexscan=off;
SET enable_bitmapscan=off;
SELECT count(*) FROM float8tmp WHERE a =  'NaN';
SELECT count(*) FROM float8tmp WHERE a >= 'NaN';
SELECT count(*) FROM float8tmp WHERE a >  'Infinity';

SET enable_seqscan=off;
SET enable_indexscan=on;
SET enable_bitmapscan=on;
SELECT count(*) FROM float8tmp WHERE a =  'NaN';
SELECT count(*) FROM float8tmp WHERE a >= 'NaN';
SELECT count(*) FROM float8tmp WHERE a >  'Infinity';
