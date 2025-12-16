-- this should work
CREATE TABLE test1 (
    did     integer,
    name    varchar(40),
    PRIMARY KEY(did)
);
-- this should fail
CREATE UNLOGGED TABLE test2 (
    did     integer,
    name    varchar(40),
    PRIMARY KEY(did)
);

-- this should fail
ALTER TABLE test1 SET UNLOGGED;

-- this should fail
ALTER TABLE test1 ADD COLUMN new_col integer, SET UNLOGGED;

-- this should work
ALTER TABLE test1 ADD COLUMN new_col integer;

-- this should work
ALTER TABLE test1 SET LOGGED;
