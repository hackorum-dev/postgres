--
-- Tests for procedures / CALL syntax
--

CREATE PROCEDURE test_proc1()
LANGUAGE plpgsql
AS $$
BEGIN
    NULL;
END;
$$;

CALL test_proc1();


-- error: can't return non-NULL
CREATE PROCEDURE test_proc2()
LANGUAGE plpgsql
AS $$
BEGIN
    RETURN 5;
END;
$$;


CREATE TABLE test1 (a int);

CREATE PROCEDURE test_proc3(x int)
LANGUAGE plpgsql
AS $$
BEGIN
    INSERT INTO test1 VALUES (x);
END;
$$;

CALL test_proc3(55);

SELECT * FROM test1;


-- nested CALL
TRUNCATE TABLE test1;

CREATE PROCEDURE test_proc4(y int)
LANGUAGE plpgsql
AS $$
BEGIN
    CALL test_proc3(y);
    CALL test_proc3($1);
END;
$$;

CALL test_proc4(66);

SELECT * FROM test1;


-- output arguments

CREATE PROCEDURE test_proc5(INOUT a text)
LANGUAGE plpgsql
AS $$
BEGIN
    a := a || '+' || a;
END;
$$;

CALL test_proc5('abc');


CREATE PROCEDURE test_proc6(a int, INOUT b int, INOUT c int)
LANGUAGE plpgsql
AS $$
BEGIN
    b := b * a;
    c := c * a;
END;
$$;

CALL test_proc6(2, 3, 4);


DO
LANGUAGE plpgsql
$$
DECLARE
    x int := 3;
    y int := 4;
BEGIN
    CALL test_proc6(2, x, y);
    RAISE INFO 'x = %, y = %', x, y;
END;
$$;


DO
LANGUAGE plpgsql
$$
DECLARE
    x int := 3;
    y int := 4;
BEGIN
    CALL test_proc6(2, x + 1, y);  -- error
    RAISE INFO 'x = %, y = %', x, y;
END;
$$;


DO
LANGUAGE plpgsql
$$
DECLARE
    x int := 3;
    y int := 4;
BEGIN
    FOR i IN 1..5 LOOP
        CALL test_proc6(i, x, y);
        RAISE INFO 'x = %, y = %', x, y;
    END LOOP;
END;
$$;


-- recursive with output arguments

CREATE PROCEDURE test_proc7(x int, INOUT a int, INOUT b numeric)
LANGUAGE plpgsql
AS $$
BEGIN
IF x > 1 THEN
    a := x / 10;
    b := x / 2;
    CALL test_proc7(b::int, a, b);
END IF;
END;
$$;

CALL test_proc7(100, -1, -1);


-- transition variable assignment

TRUNCATE test1;

CREATE FUNCTION triggerfunc1() RETURNS trigger
LANGUAGE plpgsql
AS $$
DECLARE
    z int := 0;
BEGIN
    CALL test_proc6(2, NEW.a, NEW.a);
    RETURN NEW;
END;
$$;

CREATE TRIGGER t1 BEFORE INSERT ON test1 EXECUTE PROCEDURE triggerfunc1();

INSERT INTO test1 VALUES (1), (2), (3);

UPDATE test1 SET a = 22 WHERE a = 2;

SELECT * FROM test1 ORDER BY a;


DROP PROCEDURE test_proc1;
DROP PROCEDURE test_proc3;
DROP PROCEDURE test_proc4;

DROP TABLE test1;

-- named parameters and defaults
CREATE PROCEDURE test_proc(a int, b int, c int DEFAULT -1)
AS $$
BEGIN
  RAISE NOTICE 'a: %, b: %, c: %', a, b, c;
END;
$$ LANGUAGE plpgsql;

CALL test_proc(10,20,30);
CALL test_proc(10,20);
CALL test_proc(c=>1, a=>3, b=>2);

DROP PROCEDURE test_proc;

CREATE PROCEDURE test_proc1(INOUT _a int, INOUT _b int)
AS $$
BEGIN
  RAISE NOTICE 'test_proc1: a: %, b: %', _a, _b;
  _a := _a * 10;
  _b := _b + 10;
END;
$$ LANGUAGE plpgsql;

CALL test_proc1(10,20);
CALL test_proc1(_b=>20, _a=>10);

DO $$
DECLARE a int; b int;
BEGIN
  a := 10; b := 30;
  CALL test_proc1(a, b);
  RAISE NOTICE 'a: %, b: %', a, b;
  a := 10; b := 30;
  CALL test_proc1(_b=>b, _a=>a);
  RAISE NOTICE 'a: %, b: %', a, b;
END
$$;

DROP PROCEDURE test_proc1;

CREATE PROCEDURE test_proc1(INOUT _a int, INOUT _b int, INOUT _c int)
AS $$
BEGIN
  RAISE NOTICE 'test_proc1: a: %, b: %, c: %', _a, _b, _c;
  _a := _a * 10;
  _b := _b + 10;
  _c := _c * -10;
END;
$$ LANGUAGE plpgsql;

DO $$
DECLARE a int; b int; c int;
BEGIN
  a := 10; b := 30; c := 50;
  CALL test_proc1(a, b, c);
  RAISE NOTICE 'a: %, b: %, c: %', a, b, c;
  a := 10; b := 30; c := 50;
  CALL test_proc1(a, _c=>c, _b=>b);
  RAISE NOTICE 'a: %, b: %, c: %', a, b, c;
END
$$;

DROP PROCEDURE test_proc1;
