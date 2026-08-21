--
-- Tests for pg_authid.rollastupdated
--
-- rollastupdated records when the most recent CREATE ROLE or ALTER ROLE
-- command for a role was executed, so that tools managing roles
-- declaratively can cheaply detect that a role may have changed.
--
-- Each command below is checked twice against the timestamp recorded
-- before it ran: first while nothing has happened yet, where no row
-- should qualify, and again afterwards, where the role should appear.
--

-- roles created during initdb have no recorded modification time
SELECT rollastupdated IS NULL AS bootstrap_role_is_null
    FROM pg_authid WHERE rolname = 'pg_read_all_data';

-- CREATE ROLE records a timestamp
CREATE ROLE regress_lastupdated_role;
SELECT rollastupdated IS NOT NULL AS create_sets_lastupdated
    FROM pg_authid WHERE rolname = 'regress_lastupdated_role';

SELECT rollastupdated AS ts_created
    FROM pg_authid WHERE rolname = 'regress_lastupdated_role' \gset

-- changing an attribute advances it
SELECT rolname FROM pg_authid
    WHERE rolname = 'regress_lastupdated_role' AND rollastupdated > :'ts_created';
ALTER ROLE regress_lastupdated_role WITH CONNECTION LIMIT 5;
SELECT rolname FROM pg_authid
    WHERE rolname = 'regress_lastupdated_role' AND rollastupdated > :'ts_created';

SELECT rollastupdated AS ts_altered
    FROM pg_authid WHERE rolname = 'regress_lastupdated_role' \gset

-- ALTER ROLE ... SET stores the setting outside pg_authid, but still
-- advances it
SELECT rolname FROM pg_authid
    WHERE rolname = 'regress_lastupdated_role' AND rollastupdated > :'ts_altered';
ALTER ROLE regress_lastupdated_role SET work_mem = '10MB';
SELECT rolname FROM pg_authid
    WHERE rolname = 'regress_lastupdated_role' AND rollastupdated > :'ts_altered';

SELECT rollastupdated AS ts_set
    FROM pg_authid WHERE rolname = 'regress_lastupdated_role' \gset

-- a rename advances it too, and the value is visible through pg_roles
SELECT rolname FROM pg_roles
    WHERE rolname = 'regress_lastupdated_role' AND rollastupdated > :'ts_set';
ALTER ROLE regress_lastupdated_role RENAME TO regress_lastupdated_role2;
SELECT rolname FROM pg_roles
    WHERE rolname = 'regress_lastupdated_role2' AND rollastupdated > :'ts_set';

DROP ROLE regress_lastupdated_role2;
