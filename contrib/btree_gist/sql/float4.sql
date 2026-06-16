-- float4 check

CREATE TABLE float4tmp (a float4);

\copy float4tmp from 'data/float4.data'

SET enable_seqscan=on;

SELECT count(*) FROM float4tmp WHERE a <  -179.0;

SELECT count(*) FROM float4tmp WHERE a <= -179.0;

SELECT count(*) FROM float4tmp WHERE a  = -179.0;

SELECT count(*) FROM float4tmp WHERE a >= -179.0;

SELECT count(*) FROM float4tmp WHERE a >  -179.0;

SELECT a, a <-> '-179.0' FROM float4tmp ORDER BY a <-> '-179.0' LIMIT 3;

CREATE INDEX float4idx ON float4tmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM float4tmp WHERE a <  -179.0::float4;

SELECT count(*) FROM float4tmp WHERE a <= -179.0::float4;

SELECT count(*) FROM float4tmp WHERE a  = -179.0::float4;

SELECT count(*) FROM float4tmp WHERE a >= -179.0::float4;

SELECT count(*) FROM float4tmp WHERE a >  -179.0::float4;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '-179.0' FROM float4tmp ORDER BY a <-> '-179.0' LIMIT 3;
SELECT a, a <-> '-179.0' FROM float4tmp ORDER BY a <-> '-179.0' LIMIT 3;

-- NaN and infinities must give identical results via seqscan and index scan;
-- NaN sorts after every non-NaN value and is equal to itself.
INSERT INTO float4tmp VALUES ('NaN'), ('Infinity'), ('-Infinity');

SET enable_seqscan=on;
SET enable_indexscan=off;
SET enable_bitmapscan=off;
SELECT count(*) FROM float4tmp WHERE a =  'NaN';
SELECT count(*) FROM float4tmp WHERE a >= 'NaN';
SELECT count(*) FROM float4tmp WHERE a >  'Infinity';

SET enable_seqscan=off;
SET enable_indexscan=on;
SET enable_bitmapscan=on;
SELECT count(*) FROM float4tmp WHERE a =  'NaN';
SELECT count(*) FROM float4tmp WHERE a >= 'NaN';
SELECT count(*) FROM float4tmp WHERE a >  'Infinity';
