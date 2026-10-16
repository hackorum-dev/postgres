-- date check

CREATE TABLE datetmp (a date);

\copy datetmp from 'data/date.data'

SET enable_seqscan=on;

SELECT count(*) FROM datetmp WHERE a <  '2001-02-13';

SELECT count(*) FROM datetmp WHERE a <= '2001-02-13';

SELECT count(*) FROM datetmp WHERE a  = '2001-02-13';

SELECT count(*) FROM datetmp WHERE a >= '2001-02-13';

SELECT count(*) FROM datetmp WHERE a >  '2001-02-13';

SELECT a, a <-> '2001-02-13' FROM datetmp ORDER BY a <-> '2001-02-13' LIMIT 3;

CREATE INDEX dateidx ON datetmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM datetmp WHERE a <  '2001-02-13'::date;

SELECT count(*) FROM datetmp WHERE a <= '2001-02-13'::date;

SELECT count(*) FROM datetmp WHERE a  = '2001-02-13'::date;

SELECT count(*) FROM datetmp WHERE a >= '2001-02-13'::date;

SELECT count(*) FROM datetmp WHERE a >  '2001-02-13'::date;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '2001-02-13' FROM datetmp ORDER BY a <-> '2001-02-13' LIMIT 3;
SELECT a, a <-> '2001-02-13' FROM datetmp ORDER BY a <-> '2001-02-13' LIMIT 3;

-- infinite dates in an index with internal pages (penalty and KNN distance)
CREATE TABLE dateinf (a date);
CREATE INDEX dateinfidx ON dateinf USING gist ( a );
INSERT INTO dateinf SELECT '2000-01-01'::date + i FROM generate_series(1, 1000) i;
INSERT INTO dateinf VALUES ('infinity'), ('-infinity');
SELECT a FROM dateinf ORDER BY a <-> '2000-01-01'::date LIMIT 3;
