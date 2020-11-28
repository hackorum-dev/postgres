-- inet check

CREATE TABLE inettmp (a inet);

\copy inettmp from 'data/inet.data'

SET enable_seqscan=on;

SELECT count(*) FROM inettmp WHERE a <  '89.225.196.191';

SELECT count(*) FROM inettmp WHERE a <= '89.225.196.191';

SELECT count(*) FROM inettmp WHERE a  = '89.225.196.191';

SELECT count(*) FROM inettmp WHERE a >= '89.225.196.191';

SELECT count(*) FROM inettmp WHERE a >  '89.225.196.191';

SELECT count(*) FROM inettmp WHERE a >  '255.255.1.0/25';

CREATE INDEX inetidx ON inettmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM inettmp WHERE a <  '89.225.196.191'::inet;

SELECT count(*) FROM inettmp WHERE a <= '89.225.196.191'::inet;

SELECT count(*) FROM inettmp WHERE a  = '89.225.196.191'::inet;

SELECT count(*) FROM inettmp WHERE a >= '89.225.196.191'::inet;

SELECT count(*) FROM inettmp WHERE a >  '89.225.196.191'::inet;

SELECT count(*) FROM inettmp WHERE a >  '255.255.1.0/25'::inet;

VACUUM ANALYZE inettmp;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM inettmp WHERE a  = '89.225.196.191'::inet;

SELECT count(*) FROM inettmp WHERE a  = '89.225.196.191'::inet;

DROP INDEX inetidx;

CREATE INDEX ON inettmp USING gist (a gist_inet_ops, a inet_ops);

-- checks for core planner bug
EXPLAIN (COSTS OFF)
SELECT count(*) FROM inettmp WHERE a  = '89.225.196.191'::inet;

SELECT count(*) FROM inettmp WHERE a  = '89.225.196.191'::inet;
