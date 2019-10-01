--
-- CHAR
--

-- This type, known internally as bpchar, is the same as varchar or text
-- except that trailing blanks are considered insignificant in comparisons
-- (so we strip them when converting to text), and we blank-pad to the
-- declared length if there is one.

SELECT char 'c' = char 'c' AS true;

--
-- Build a table for testing
--

CREATE TABLE CHAR_TBL(f1 char);

INSERT INTO CHAR_TBL (f1) VALUES ('a');

INSERT INTO CHAR_TBL (f1) VALUES ('A');

-- any of the following three input formats are acceptable
INSERT INTO CHAR_TBL (f1) VALUES ('1');

INSERT INTO CHAR_TBL (f1) VALUES (2);

INSERT INTO CHAR_TBL (f1) VALUES ('3');

-- zero-length char
INSERT INTO CHAR_TBL (f1) VALUES ('');

-- try char's of greater than 1 length
INSERT INTO CHAR_TBL (f1) VALUES ('cd');
INSERT INTO CHAR_TBL (f1) VALUES ('c     ');


SELECT '' AS seven, * FROM CHAR_TBL;

SELECT '' AS six, c.*
   FROM CHAR_TBL c
   WHERE c.f1 <> 'a';

SELECT '' AS one, c.*
   FROM CHAR_TBL c
   WHERE c.f1 = 'a';

SELECT '' AS five, c.*
   FROM CHAR_TBL c
   WHERE c.f1 < 'a';

SELECT '' AS six, c.*
   FROM CHAR_TBL c
   WHERE c.f1 <= 'a';

SELECT '' AS one, c.*
   FROM CHAR_TBL c
   WHERE c.f1 > 'a';

SELECT '' AS two, c.*
   FROM CHAR_TBL c
   WHERE c.f1 >= 'a';

DROP TABLE CHAR_TBL;

--
-- Now test longer arrays of char
--

CREATE TABLE CHAR_TBL(f1 char(4));

INSERT INTO CHAR_TBL (f1) VALUES ('a');
INSERT INTO CHAR_TBL (f1) VALUES ('ab');
INSERT INTO CHAR_TBL (f1) VALUES ('abcd');
INSERT INTO CHAR_TBL (f1) VALUES ('abcde');
INSERT INTO CHAR_TBL (f1) VALUES ('abcd    ');

SELECT '' AS four, * FROM CHAR_TBL;

--
-- Check regex and LIKE comparisons
--

SELECT * FROM CHAR_TBL WHERE f1 LIKE 'a';
SELECT * FROM CHAR_TBL WHERE f1 LIKE 'ab%';
SELECT * FROM CHAR_TBL WHERE f1 ILIKE 'AB%';
SELECT * FROM CHAR_TBL WHERE f1 ~ 'b';
SELECT * FROM CHAR_TBL WHERE f1 ~ '^ab';
SELECT * FROM CHAR_TBL WHERE f1 ~ '^ab$';
