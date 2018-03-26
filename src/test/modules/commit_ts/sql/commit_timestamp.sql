--
-- Commit Timestamp
--
SHOW track_commit_timestamp;
CREATE TABLE committs_test(id serial, ts timestamptz default now());

INSERT INTO committs_test DEFAULT VALUES;
INSERT INTO committs_test DEFAULT VALUES;
INSERT INTO committs_test DEFAULT VALUES;

SELECT pg_replication_origin_create('test_origin');
SELECT pg_replication_origin_session_setup('test_origin');
BEGIN;
SELECT pg_replication_origin_xact_setup('1/1', '1990-01-01');
INSERT INTO committs_test DEFAULT VALUES;
COMMIT;
SELECT pg_replication_origin_session_reset();

SELECT id,
       pg_xact_commit_timestamp(xmin) >= ts,
       pg_xact_commit_timestamp(xmin) <= now(),
       pg_xact_commit_timestamp(xmin) - ts < '60s' -- 60s should give a lot of reserve
FROM committs_test
ORDER BY id;

SELECT roident = 0, roident = 1
FROM committs_test, pg_xact_commit_timestamp_origin(xmin)
ORDER BY id;

DROP TABLE committs_test;

SELECT pg_xact_commit_timestamp('0'::xid);
SELECT pg_xact_commit_timestamp('1'::xid);
SELECT pg_xact_commit_timestamp('2'::xid);

SELECT x.xid::text::bigint > 0, x.timestamp > '-infinity'::timestamptz, x.timestamp <= now(), roident = 0 FROM pg_last_committed_xact() x;
