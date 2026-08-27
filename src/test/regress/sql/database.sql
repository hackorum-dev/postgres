CREATE DATABASE regression_tbd
	ENCODING utf8 LC_COLLATE "C" LC_CTYPE "C" TEMPLATE template0;

-- template1 is created during bootstrap, so it has no modification time,
-- whereas CREATE DATABASE records one
SELECT datupdated IS NULL AS template1_is_null
    FROM pg_database WHERE datname = 'template1';
SELECT datupdated IS NOT NULL AS create_sets_updated
    FROM pg_database WHERE datname = 'regression_tbd';
SELECT datupdated AS ts_created
    FROM pg_database WHERE datname = 'regression_tbd' \gset

-- each of the commands below advances datupdated; the check before
-- each one shows that the comparison discriminates
SELECT datname FROM pg_database
    WHERE datname = 'regression_tbd' AND datupdated > :'ts_created';
ALTER DATABASE regression_tbd RENAME TO regression_utf8;
SELECT datname FROM pg_database
    WHERE datname = 'regression_utf8' AND datupdated > :'ts_created';
SELECT datupdated AS ts_renamed
    FROM pg_database WHERE datname = 'regression_utf8' \gset

ALTER DATABASE regression_utf8 SET TABLESPACE regress_tblspace;
ALTER DATABASE regression_utf8 SET TABLESPACE pg_default;
SELECT datname FROM pg_database
    WHERE datname = 'regression_utf8' AND datupdated > :'ts_renamed';
SELECT datupdated AS ts_moved
    FROM pg_database WHERE datname = 'regression_utf8' \gset

ALTER DATABASE regression_utf8 CONNECTION_LIMIT 123;
SELECT datname FROM pg_database
    WHERE datname = 'regression_utf8' AND datupdated > :'ts_moved';
SELECT datupdated AS ts_altered
    FROM pg_database WHERE datname = 'regression_utf8' \gset

-- ALTER DATABASE ... SET stores the setting outside pg_database, but still
-- advances datupdated
SELECT datname FROM pg_database
    WHERE datname = 'regression_utf8' AND datupdated > :'ts_altered';
ALTER DATABASE regression_utf8 SET work_mem = '10MB';
SELECT datname FROM pg_database
    WHERE datname = 'regression_utf8' AND datupdated > :'ts_altered';
ALTER DATABASE regression_utf8 RESET work_mem;

-- Test PgDatabaseToastTable.  Doing this with GRANT would be slow.
BEGIN;
UPDATE pg_database
SET datacl = array_fill(makeaclitem(10, 10, 'USAGE', false), ARRAY[5e5::int])
WHERE datname = 'regression_utf8';
-- load catcache entry, if nothing else does
ALTER DATABASE regression_utf8 RENAME TO regression_rename_rolled_back;
ROLLBACK;

CREATE ROLE regress_datdba_before;
CREATE ROLE regress_datdba_after;
SELECT datupdated AS ts_before_owner
    FROM pg_database WHERE datname = 'regression_utf8' \gset
ALTER DATABASE regression_utf8 OWNER TO regress_datdba_before;
SELECT datname FROM pg_database
    WHERE datname = 'regression_utf8' AND datupdated > :'ts_before_owner';
REASSIGN OWNED BY regress_datdba_before TO regress_datdba_after;

DROP DATABASE regression_utf8;
DROP ROLE regress_datdba_before;
DROP ROLE regress_datdba_after;
