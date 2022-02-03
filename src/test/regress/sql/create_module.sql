CREATE TABLE cm_test (a int, b text);

CREATE MODULE mtest1
  CREATE FUNCTION m1testa() RETURNS text
     LANGUAGE sql
     RETURN '1x'
  CREATE FUNCTION m1testb() RETURNS text
     LANGUAGE sql
     RETURN '1y';

CREATE SCHEMA temp_mod_test;
GRANT ALL ON SCHEMA temp_mod_test TO public;

CREATE MODULE temp_mod_test.mtest2
  CREATE PROCEDURE m2testa(x text)
  LANGUAGE SQL
  AS $$
  INSERT INTO cm_test VALUES (1, x);
  $$
  CREATE FUNCTION m2testb() RETURNS text
     LANGUAGE sql
     RETURN '2y';

CREATE MODULE mtest3
  CREATE FUNCTION m3testa() RETURNS text
     LANGUAGE sql
     RETURN '3x';

SELECT mtest1.m1testa();
SELECT mtest1.m1testb();

SELECT public.mtest1.m1testa();
SELECT public.mtest1.m1testb();

SELECT temp_mod_test.mtest2.m2testb();

SELECT temp_mod_test.mtest2.m2testa('x');  -- error
CALL temp_mod_test.mtest2.m2testa('a');  -- ok
CALL temp_mod_test.mtest2.m2testa('xy' || 'zzy');  -- ok, constant-folded arg

ALTER MODULE mtest1 CREATE PROCEDURE m1testc(x text)
  LANGUAGE SQL
  AS $$
  INSERT INTO cm_test VALUES (2, x);
  $$;

CALL mtest1.m1testc('a');  -- ok

-- create and modify functions in modules

ALTER MODULE mtest1 CREATE FUNCTION m1testd() RETURNS text
     LANGUAGE sql
     RETURN 'm1testd';

SELECT mtest1.m1testd();

ALTER MODULE mtest1 CREATE OR REPLACE FUNCTION m1testd() RETURNS text
     LANGUAGE sql
     RETURN 'm1testd replaced';

SELECT mtest1.m1testd();

-- grant and revoke

DROP ROLE IF EXISTS regress_priv_user1;
CREATE USER regress_priv_user1;
ALTER ROLE regress_priv_user1 NOINHERIT;
GRANT CREATE ON SCHEMA public TO regress_priv_user1;

REVOKE ON MODULE mtest1 REFERENCES ON FUNCTION m1testa() FROM public;
REVOKE ON MODULE mtest1 REFERENCES ON FUNCTION m1testb() FROM public;

GRANT ON MODULE mtest1 REFERENCES ON FUNCTION m1testa() TO regress_priv_user1;
REVOKE ON MODULE mtest1 REFERENCES ON FUNCTION m1testb() FROM regress_priv_user1;

SET SESSION AUTHORIZATION regress_priv_user1;
SELECT mtest1.m1testa();  -- ok
SELECT mtest1.m1testb();  -- error
RESET SESSION AUTHORIZATION;

REVOKE ON MODULE mtest1 REFERENCES ON FUNCTION m1testa() FROM regress_priv_user1;
GRANT ON MODULE mtest1 REFERENCES ON FUNCTION m1testb() TO regress_priv_user1;
SET SESSION AUTHORIZATION regress_priv_user1;
SELECT mtest1.m1testa();  -- error
SELECT mtest1.m1testb();  -- ok
RESET SESSION AUTHORIZATION;

REVOKE ON MODULE mtest1 REFERENCES ON ALL FUNCTIONS FROM regress_priv_user1;
REVOKE ON MODULE mtest1 CREATE FROM regress_priv_user1;
SET SESSION AUTHORIZATION regress_priv_user1;
SELECT mtest1.m1testa(); -- error
SELECT mtest1.m1testb(); -- error
ALTER MODULE mtest1 CREATE OR REPLACE FUNCTION m1testf() RETURNS text
     LANGUAGE sql
     RETURN 'm1testf'; -- error
RESET SESSION AUTHORIZATION;

GRANT ON MODULE mtest1 REFERENCES ON ALL FUNCTIONS TO regress_priv_user1;
GRANT ON MODULE mtest1 CREATE TO regress_priv_user1;
SET SESSION AUTHORIZATION regress_priv_user1;
SELECT mtest1.m1testa();  -- ok
SELECT mtest1.m1testb();  -- ok
ALTER MODULE mtest1 CREATE OR REPLACE FUNCTION m1testf() RETURNS text
     LANGUAGE sql
     RETURN 'm1testf';  -- ok
RESET SESSION AUTHORIZATION;
SELECT mtest1.m1testf();  -- ok

-- rename module and functions in module
ALTER MODULE mtest1 RENAME TO mtest1renamed;

SELECT mtest1.m1testd();  -- error
SELECT mtest1renamed.m1testd();  -- ok

ALTER FUNCTION mtest1renamed.m1testd() RENAME TO m1testdrenamed;

SELECT mtest1renamed.m1testd();  -- error
SELECT mtest1renamed.m1testdrenamed();  -- ok

-- drop
DROP PROCEDURE mtest1renamed.m1testc(text);
DROP FUNCTION temp_mod_test.mtest2.m2testb();
DROP MODULE mtest1renamed; -- error

-- cleanup
DROP MODULE mtest1renamed CASCADE;
DROP MODULE temp_mod_test.mtest2 CASCADE;
DROP OWNED BY regress_priv_user1;
DROP ROLE regress_priv_user1;

DROP SCHEMA temp_mod_test;
DROP TABLE cm_test;
