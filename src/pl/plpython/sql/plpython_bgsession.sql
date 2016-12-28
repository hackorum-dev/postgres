CREATE TABLE test1 (a int, b text);


-- test of independent commit/rollback

CREATE FUNCTION bgsession_test1() RETURNS integer
LANGUAGE plpythonu
AS $$
with plpy.BackgroundSession() as a:
    for i in range(0, 10):
        a.execute("BEGIN")
        a.execute("INSERT INTO test1 (a) VALUES (%d)" % i)
        if i % 2 == 0:
            a.execute("COMMIT")
        else:
            a.execute("ROLLBACK")
$$;

SELECT bgsession_test1();

SELECT * FROM test1;


-- test query result

CREATE FUNCTION bgsession_test2() RETURNS integer
LANGUAGE plpythonu
AS $$
with plpy.BackgroundSession() as a:
        a.execute("BEGIN")
        a.execute("INSERT INTO test1 (a) VALUES (11)")
        rv = a.execute("SELECT * FROM test1")
        plpy.info(rv)
        a.execute("ROLLBACK")
$$;

SELECT bgsession_test2();

SELECT * FROM test1;


-- test notice and error forwarding

CREATE FUNCTION bgsession_test3() RETURNS integer
LANGUAGE plpythonu
AS $$
with plpy.BackgroundSession() as a:
    a.execute("DO $_$ BEGIN RAISE NOTICE 'notice'; END $_$")
    a.execute("DO $_$ BEGIN RAISE EXCEPTION 'error'; END $_$")
$$;

SELECT bgsession_test3();


-- test prohibit changing client encoding

CREATE FUNCTION bgsession_test4() RETURNS integer
LANGUAGE plpythonu
AS $$
with plpy.BackgroundSession() as a:
    a.execute("SET client_encoding TO SJIS")
$$;

SELECT bgsession_test4();


-- test prepared statements

CREATE FUNCTION bgsession_test5() RETURNS integer
LANGUAGE plpythonu
AS $$
with plpy.BackgroundSession() as a:
    plan = a.prepare("INSERT INTO test1 (a, b) VALUES ($1, $2)", ["int4", "text"])
    a.execute_prepared(plan, [1, "one"])
    a.execute_prepared(plan, [2, "two"])
$$;

TRUNCATE test1;

SELECT bgsession_test5();

SELECT * FROM test1;


-- test result from prepared query

CREATE FUNCTION bgsession_test7() RETURNS integer
LANGUAGE plpythonu
AS $$
with plpy.BackgroundSession() as a:
        a.execute("BEGIN")
        plan = a.prepare("INSERT INTO test1 (a) VALUES ($1)", ["int4"])
        a.execute_prepared(plan, [11])
        plan = a.prepare("SELECT * FROM test1")
        rv = a.execute_prepared(plan, [])
        plpy.info(rv)
        a.execute("ROLLBACK")
$$;

TRUNCATE test1;

SELECT bgsession_test7();

SELECT * FROM test1;


-- test error when not closing transaction

CREATE FUNCTION bgsession_test8() RETURNS integer
LANGUAGE plpythonu
AS $$
with plpy.BackgroundSession() as a:
        a.execute("BEGIN")
$$;

SELECT bgsession_test8();


-- test saving session

CREATE FUNCTION bgsession_test9a() RETURNS integer
LANGUAGE plpythonu
AS $$
bg = plpy.BackgroundSession()
GD['bg'] = bg
bg.execute("BEGIN")
bg.execute("INSERT INTO test1 VALUES (1)")

return 1
$$;

CREATE FUNCTION bgsession_test9b() RETURNS integer
LANGUAGE plpythonu
AS $$
bg = GD['bg']
bg.execute("INSERT INTO test1 VALUES (2)")
bg.execute("COMMIT")
bg.close()

return 2
$$;

TRUNCATE test1;

SELECT bgsession_test9a();
SELECT bgsession_test9b();

SELECT * FROM test1;


-- test error handling

CREATE FUNCTION bgsession_test10() RETURNS integer
LANGUAGE plpythonu
AS $$
with plpy.BackgroundSession() as bg:
    try:
        bg.execute("SELECT error")
    except Exception, ex:
        plpy.notice("caught exception: %s" % ex)

    try:
        bg.prepare("SELECT error")
    except Exception, ex:
        plpy.notice("caught exception: %s" % ex)

    try:
        plan = bg.prepare("SELECT $1", ["int4"])
        bg.execute_prepared(plan, ["foo"])
    except plpy.Error, ex:
        plpy.notice("caught exception: %s" % ex)
$$;

SELECT bgsession_test10();


DROP TABLE test1;
