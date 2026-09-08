-- A table_rewrite trigger must not access a relation whose catalog definition
-- has changed but whose tuples have not yet been rewritten.
CREATE TABLE rewrite_target (a int, b text);
INSERT INTO rewrite_target VALUES (1, 'original');
CREATE TABLE rewrite_log (relid oid, reason int);

CREATE FUNCTION rewrite_access() RETURNS event_trigger
LANGUAGE plpgsql AS $$
BEGIN
  EXECUTE current_setting('regress.rewrite_command');
END;
$$;
CREATE EVENT TRIGGER rewrite_access ON table_rewrite
  EXECUTE FUNCTION rewrite_access();

-- The original corruption case.  The whole ALTER must roll back.
SET regress.rewrite_command = 'INSERT INTO rewrite_target VALUES (999, ''rw'')';
ALTER TABLE rewrite_target ALTER COLUMN a TYPE bigint;
SELECT a, b, pg_typeof(a) FROM rewrite_target;

-- Reading the table is unsafe too.  Exercise planning and cached plans.
SET regress.rewrite_command = 'SELECT b FROM rewrite_target';
ALTER TABLE rewrite_target ALTER COLUMN a TYPE bigint;
PREPARE rewrite_insert AS INSERT INTO rewrite_target VALUES (999, 'cached');
SET regress.rewrite_command = 'EXECUTE rewrite_insert';
ALTER TABLE rewrite_target ALTER COLUMN a TYPE bigint;
DEALLOCATE rewrite_insert;

SET regress.rewrite_command = 'UPDATE rewrite_target SET b = ''changed''';
ALTER TABLE rewrite_target ALTER COLUMN a TYPE bigint;
SET regress.rewrite_command = 'DELETE FROM rewrite_target';
ALTER TABLE rewrite_target ALTER COLUMN a TYPE bigint;
-- Use server-side COPY so SPI's client-COPY restriction is not the guard.
SET regress.rewrite_command = 'COPY rewrite_target FROM ''nonexistent_rewrite_input''';
ALTER TABLE rewrite_target ALTER COLUMN a TYPE bigint;
SET regress.rewrite_command = 'TRUNCATE rewrite_target';
ALTER TABLE rewrite_target ALTER COLUMN a TYPE bigint;
SELECT a, b, pg_typeof(a) FROM rewrite_target;

-- Catalog inspection and writing an unrelated audit table remain allowed.
SET regress.rewrite_command = 'INSERT INTO rewrite_log SELECT oid, pg_event_trigger_table_rewrite_reason() FROM pg_class WHERE oid = pg_event_trigger_table_rewrite_oid()';
ALTER TABLE rewrite_target ALTER COLUMN a TYPE bigint;
SELECT a, b, pg_typeof(a) FROM rewrite_target;
SELECT relid::regclass, reason FROM rewrite_log;

-- Protect other tables in the same work queue, including inheritance children.
CREATE TABLE rewrite_parent (a int, b text);
CREATE TABLE rewrite_child () INHERITS (rewrite_parent);
INSERT INTO rewrite_child VALUES (2, 'child');
SET regress.rewrite_command = 'INSERT INTO rewrite_child VALUES (999, ''rw'')';
ALTER TABLE rewrite_parent ALTER COLUMN a TYPE bigint;
SELECT a, b, pg_typeof(a) FROM rewrite_child;

-- Nested rewrites must preserve the outer guard, including after an error
-- caught by a PL/pgSQL exception handler.  The unrelated inner table is usable
-- again once its ALTER has finished.
CREATE TABLE rewrite_inner (a int);
CREATE OR REPLACE FUNCTION rewrite_access() RETURNS event_trigger
LANGUAGE plpgsql AS $$
BEGIN
  IF pg_event_trigger_table_rewrite_oid() = 'rewrite_parent'::regclass THEN
    BEGIN
      ALTER TABLE rewrite_inner ALTER COLUMN a TYPE bigint;
    EXCEPTION WHEN object_in_use THEN
      RAISE NOTICE 'nested access rejected';
    END;
    -- The failed inner ALTER must have restored both its catalog and guard.
    INSERT INTO rewrite_inner VALUES (7);
    BEGIN
      INSERT INTO rewrite_child VALUES (999, 'outer');
    EXCEPTION WHEN object_in_use THEN
      RAISE NOTICE 'outer access still rejected';
    END;
  ELSIF pg_event_trigger_table_rewrite_oid() = 'rewrite_inner'::regclass THEN
    -- This is protected by the outer command, not the inner work queue.
    INSERT INTO rewrite_child VALUES (999, 'nested');
  END IF;
END;
$$;
ALTER TABLE rewrite_parent ALTER COLUMN a TYPE bigint;
SELECT a, b, pg_typeof(a) FROM rewrite_child;
SELECT a, pg_typeof(a) FROM rewrite_inner;

-- A successful nested rewrite also restores the previous guard.
CREATE OR REPLACE FUNCTION rewrite_access() RETURNS event_trigger
LANGUAGE plpgsql AS $$
BEGIN
  IF pg_event_trigger_table_rewrite_oid() = 'rewrite_parent'::regclass THEN
    ALTER TABLE rewrite_inner ALTER COLUMN a TYPE bigint;
    INSERT INTO rewrite_inner VALUES (8);
    BEGIN
      PERFORM b FROM rewrite_child;
    EXCEPTION WHEN object_in_use THEN
      RAISE NOTICE 'outer access still rejected after nested success';
    END;
  END IF;
END;
$$;
ALTER TABLE rewrite_parent ALTER COLUMN a TYPE numeric;
SELECT a, b, pg_typeof(a) FROM rewrite_child;
SELECT a, pg_typeof(a) FROM rewrite_inner ORDER BY a;

DROP EVENT TRIGGER rewrite_access;
DROP FUNCTION rewrite_access();
DROP TABLE rewrite_child, rewrite_parent, rewrite_inner;
DROP TABLE rewrite_target, rewrite_log;
RESET regress.rewrite_command;
