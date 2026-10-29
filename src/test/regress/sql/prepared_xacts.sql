SELECT current_setting('max_prepared_transactions')::integer < 2 AS skip_test \gset
\if :skip_test
\quit
\endif

--
-- PREPARED TRANSACTIONS (two-phase commit)
--
-- We can't readily test persistence of prepared xacts within the
-- regression script framework, unfortunately.  Note that a crash
-- isn't really needed ... stopping and starting the postmaster would
-- be enough, but we can't even do that here.


-- create a simple table that we'll use in the tests
CREATE TABLE pxtest1 (foobar VARCHAR(10));

INSERT INTO pxtest1 VALUES ('aaa');


-- Test PREPARE TRANSACTION
BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
UPDATE pxtest1 SET foobar = 'bbb' WHERE foobar = 'aaa';
SELECT * FROM pxtest1;
PREPARE TRANSACTION 'regress_foo1';

SELECT * FROM pxtest1;

-- Test pg_prepared_xacts system view
SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;

-- Test ROLLBACK PREPARED
ROLLBACK PREPARED 'regress_foo1';

SELECT * FROM pxtest1;

SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;


-- Test COMMIT PREPARED
BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
INSERT INTO pxtest1 VALUES ('ddd');
SELECT * FROM pxtest1;
PREPARE TRANSACTION 'regress_foo2';

SELECT * FROM pxtest1;

COMMIT PREPARED 'regress_foo2';

SELECT * FROM pxtest1;

-- Test duplicate gids
BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
UPDATE pxtest1 SET foobar = 'eee' WHERE foobar = 'ddd';
SELECT * FROM pxtest1;
PREPARE TRANSACTION 'regress_foo3';

SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;

BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
INSERT INTO pxtest1 VALUES ('fff');

-- This should fail, because the gid foo3 is already in use
PREPARE TRANSACTION 'regress_foo3';

SELECT * FROM pxtest1;

ROLLBACK PREPARED 'regress_foo3';

SELECT * FROM pxtest1;

-- Test serialization failure (SSI)
BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
UPDATE pxtest1 SET foobar = 'eee' WHERE foobar = 'ddd';
SELECT * FROM pxtest1;
PREPARE TRANSACTION 'regress_foo4';

SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;

BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
SELECT * FROM pxtest1;

-- This should fail, because the two transactions have a write-skew anomaly
INSERT INTO pxtest1 VALUES ('fff');
PREPARE TRANSACTION 'regress_foo5';

SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;

ROLLBACK PREPARED 'regress_foo4';

SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;

-- Clean up
DROP TABLE pxtest1;

-- Test detection of session-level and xact-level locks on same object
BEGIN;
SELECT pg_advisory_lock(1);
SELECT pg_advisory_xact_lock_shared(1);
PREPARE TRANSACTION 'regress_foo6';  -- fails

-- Test subtransactions
BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
  CREATE TABLE pxtest2 (a int);
  INSERT INTO pxtest2 VALUES (1);
  SAVEPOINT a;
    INSERT INTO pxtest2 VALUES (2);
  ROLLBACK TO a;
  SAVEPOINT b;
  INSERT INTO pxtest2 VALUES (3);
PREPARE TRANSACTION 'regress_sub1';

CREATE TABLE pxtest3(fff int);

-- Test shared invalidation
BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
  DROP TABLE pxtest3;
  CREATE TABLE pxtest4 (a int);
  INSERT INTO pxtest4 VALUES (1);
  INSERT INTO pxtest4 VALUES (2);
  DECLARE foo CURSOR FOR SELECT * FROM pxtest4;
  -- Fetch 1 tuple, keeping the cursor open
  FETCH 1 FROM foo;
PREPARE TRANSACTION 'regress_sub2';

-- No such cursor
FETCH 1 FROM foo;

-- Table doesn't exist, the creation hasn't been committed yet
SELECT * FROM pxtest2;

-- There should be two prepared transactions
SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;

-- pxtest3 should be locked because of the pending DROP
begin;
lock table pxtest3 in access share mode nowait;
rollback;

-- Disconnect, we will continue testing in a different backend
\c -

-- There should still be two prepared transactions
SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;

-- pxtest3 should still be locked because of the pending DROP
begin;
lock table pxtest3 in access share mode nowait;
rollback;

-- Commit table creation
COMMIT PREPARED 'regress_sub1';
\d pxtest2
SELECT * FROM pxtest2;

-- There should be one prepared transaction
SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;

-- Commit table drop
COMMIT PREPARED 'regress_sub2';
SELECT * FROM pxtest3;

-- There should be no prepared transactions
SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;


-- Test row-level locks held by prepared transactions
CREATE TABLE pxtest_rowlock (id int PRIMARY KEY, data text);
INSERT INTO pxtest_rowlock VALUES (1, 'test data');

BEGIN;
SELECT * FROM pxtest_rowlock WHERE id = 1 FOR SHARE;
PREPARE TRANSACTION 'regress_p1';

-- Should fail because the row is locked
SELECT * FROM pxtest_rowlock WHERE id = 1 FOR UPDATE NOWAIT;

-- Test prepared transactions that participate in multixacts. For
-- that, lock the same row again, creating a multixid.
BEGIN;
SELECT * FROM pxtest_rowlock WHERE id = 1 FOR SHARE;
PREPARE TRANSACTION 'regress_p2';

-- Should fail because the row is locked
SELECT * FROM pxtest_rowlock WHERE id = 1 FOR UPDATE NOWAIT;

ROLLBACK PREPARED 'regress_p1';
ROLLBACK PREPARED 'regress_p2';

-- Clean up
DROP TABLE pxtest2;
-- pxtest3 was already dropped
DROP TABLE pxtest4;
DROP TABLE pxtest_rowlock;

-- Prepared transaction ownership follows inherited role privileges.
CREATE ROLE regress_prepare_owner;
CREATE ROLE regress_prepare_member;
CREATE ROLE regress_prepare_indirect;
CREATE ROLE regress_prepare_noinherit;
CREATE ROLE regress_prepare_setonly;
CREATE ROLE regress_prepare_other;
GRANT regress_prepare_owner TO regress_prepare_member WITH INHERIT TRUE, SET FALSE;
GRANT regress_prepare_member TO regress_prepare_indirect WITH INHERIT TRUE, SET FALSE;
GRANT regress_prepare_owner TO regress_prepare_noinherit WITH INHERIT FALSE, SET FALSE;
GRANT regress_prepare_owner TO regress_prepare_setonly WITH INHERIT FALSE, SET TRUE;
CREATE TABLE pxtest_inherit (a int);
GRANT INSERT ON pxtest_inherit TO regress_prepare_owner;

-- The preparing role can still finish its own transactions.
SET ROLE regress_prepare_owner;
BEGIN;
INSERT INTO pxtest_inherit VALUES (0);
PREPARE TRANSACTION 'regress_inherit_commit';
COMMIT PREPARED 'regress_inherit_commit';
BEGIN;
INSERT INTO pxtest_inherit VALUES (1);
PREPARE TRANSACTION 'regress_inherit_rollback';
ROLLBACK PREPARED 'regress_inherit_rollback';

BEGIN;
INSERT INTO pxtest_inherit VALUES (2);
PREPARE TRANSACTION 'regress_inherit_commit';
BEGIN;
INSERT INTO pxtest_inherit VALUES (3);
PREPARE TRANSACTION 'regress_inherit_rollback';
RESET ROLE;

-- Unrelated roles and non-inherited memberships do not confer ownership.
SET ROLE regress_prepare_other;
COMMIT PREPARED 'regress_inherit_commit';  -- fails
ROLLBACK PREPARED 'regress_inherit_rollback';  -- fails
RESET ROLE;
SET ROLE regress_prepare_noinherit;
COMMIT PREPARED 'regress_inherit_commit';  -- fails
ROLLBACK PREPARED 'regress_inherit_rollback';  -- fails
RESET ROLE;
SET ROLE regress_prepare_setonly;
COMMIT PREPARED 'regress_inherit_commit';  -- fails without SET ROLE
ROLLBACK PREPARED 'regress_inherit_rollback';  -- fails without SET ROLE
RESET ROLE;

-- Inherited privileges suffice even when SET ROLE is not allowed.
SET ROLE regress_prepare_member;
SELECT pg_has_role(current_user, 'regress_prepare_owner', 'USAGE') AS inherit,
       pg_has_role(current_user, 'regress_prepare_owner', 'SET') AS can_set_role;
COMMIT PREPARED 'regress_inherit_commit';
ROLLBACK PREPARED 'regress_inherit_rollback';
RESET ROLE;

-- Indirect inheritance works without a SET ROLE path as well.
SET ROLE regress_prepare_owner;
BEGIN;
INSERT INTO pxtest_inherit VALUES (4);
PREPARE TRANSACTION 'regress_inherit_commit';
BEGIN;
INSERT INTO pxtest_inherit VALUES (5);
PREPARE TRANSACTION 'regress_inherit_rollback';
RESET ROLE;
SET ROLE regress_prepare_indirect;
COMMIT PREPARED 'regress_inherit_commit';
ROLLBACK PREPARED 'regress_inherit_rollback';
RESET ROLE;

-- Revoking and restoring the grant invalidates cached privileges.
SET ROLE regress_prepare_owner;
BEGIN;
INSERT INTO pxtest_inherit VALUES (6);
PREPARE TRANSACTION 'regress_inherit_commit';
BEGIN;
INSERT INTO pxtest_inherit VALUES (7);
PREPARE TRANSACTION 'regress_inherit_rollback';
RESET ROLE;
REVOKE regress_prepare_owner FROM regress_prepare_member;
SET ROLE regress_prepare_member;
COMMIT PREPARED 'regress_inherit_commit';  -- fails after REVOKE
ROLLBACK PREPARED 'regress_inherit_rollback';  -- fails after REVOKE
RESET ROLE;
GRANT regress_prepare_owner TO regress_prepare_member WITH INHERIT TRUE, SET FALSE;
SET ROLE regress_prepare_member;
COMMIT PREPARED 'regress_inherit_commit';
ROLLBACK PREPARED 'regress_inherit_rollback';
RESET ROLE;

-- A non-inheriting member can explicitly become the preparing role.
SET ROLE regress_prepare_owner;
BEGIN;
INSERT INTO pxtest_inherit VALUES (8);
PREPARE TRANSACTION 'regress_inherit_commit';
BEGIN;
INSERT INTO pxtest_inherit VALUES (9);
PREPARE TRANSACTION 'regress_inherit_rollback';
RESET ROLE;
SET ROLE regress_prepare_setonly;
SET ROLE regress_prepare_owner;
COMMIT PREPARED 'regress_inherit_commit';
ROLLBACK PREPARED 'regress_inherit_rollback';
RESET ROLE;

-- Superusers retain their existing ability to finish any transaction.
SET ROLE regress_prepare_owner;
BEGIN;
INSERT INTO pxtest_inherit VALUES (10);
PREPARE TRANSACTION 'regress_inherit_commit';
BEGIN;
INSERT INTO pxtest_inherit VALUES (11);
PREPARE TRANSACTION 'regress_inherit_rollback';
RESET ROLE;
COMMIT PREPARED 'regress_inherit_commit';
ROLLBACK PREPARED 'regress_inherit_rollback';

-- Membership in an unrelated ordinary role does not grant superuser powers.
BEGIN;
INSERT INTO pxtest_inherit VALUES (12);
PREPARE TRANSACTION 'regress_inherit_commit';
BEGIN;
INSERT INTO pxtest_inherit VALUES (13);
PREPARE TRANSACTION 'regress_inherit_rollback';
SET ROLE regress_prepare_member;
COMMIT PREPARED 'regress_inherit_commit';  -- fails
ROLLBACK PREPARED 'regress_inherit_rollback';  -- fails
RESET ROLE;
COMMIT PREPARED 'regress_inherit_commit';
ROLLBACK PREPARED 'regress_inherit_rollback';

SELECT * FROM pxtest_inherit ORDER BY a;
SELECT gid FROM pg_prepared_xacts WHERE gid LIKE 'regress_inherit_%';
DROP TABLE pxtest_inherit;
DROP ROLE regress_prepare_other;
DROP ROLE regress_prepare_setonly;
DROP ROLE regress_prepare_noinherit;
DROP ROLE regress_prepare_indirect;
DROP ROLE regress_prepare_member;
DROP ROLE regress_prepare_owner;
